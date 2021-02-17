#include "PostgreSQLReplicationHandler.h"

#include <DataStreams/PostgreSQLBlockInputStream.h>
#include <Databases/PostgreSQL/fetchPostgreSQLTableStructure.h>
#include <Storages/PostgreSQL/StoragePostgreSQLReplica.h>

#include <Common/setThreadName.h>
#include <DataStreams/copyData.h>
#include <Interpreters/InterpreterInsertQuery.h>
#include <Poco/File.h>


namespace DB
{

static const auto reschedule_ms = 500;

PostgreSQLReplicationHandler::PostgreSQLReplicationHandler(
    const std::string & database_name_,
    const std::string & conn_str,
    const std::string & metadata_path_,
    std::shared_ptr<Context> context_,
    const size_t max_block_size_,
    const String tables_list_)
    : log(&Poco::Logger::get("PostgreSQLReplicaHandler"))
    , context(context_)
    , database_name(database_name_)
    , connection_str(conn_str)
    , metadata_path(metadata_path_)
    , max_block_size(max_block_size_)
    , tables_list(tables_list_)
    , connection(std::make_shared<PostgreSQLConnection>(conn_str))
{
    replication_slot = fmt::format("{}_ch_replication_slot", database_name);
    publication_name = fmt::format("{}_ch_publication", database_name);

    startup_task = context->getSchedulePool().createTask("PostgreSQLReplicaStartup", [this]{ waitConnectionAndStart(); });
    startup_task->deactivate();
}


void PostgreSQLReplicationHandler::addStorage(const std::string & table_name, StoragePostgreSQLReplica * storage)
{
    storages[table_name] = storage;
}


void PostgreSQLReplicationHandler::startup()
{
    startup_task->activateAndSchedule();
}


void PostgreSQLReplicationHandler::waitConnectionAndStart()
{
    try
    {
        /// Will throw pqxx::broken_connection if no connection at the moment
        connection->conn();

        startSynchronization();
    }
    catch (const pqxx::broken_connection & pqxx_error)
    {
        LOG_ERROR(log, "Unable to set up connection. Reconnection attempt will continue. Error message: {}",
                pqxx_error.what());

        startup_task->scheduleAfter(reschedule_ms);
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}


void PostgreSQLReplicationHandler::shutdown()
{
    if (consumer)
        consumer->stopSynchronization();
}


void PostgreSQLReplicationHandler::startSynchronization()
{
    createPublicationIfNeeded(connection->conn());

    auto replication_connection = std::make_shared<PostgreSQLConnection>(fmt::format("{} replication=database", connection->conn_str()));
    replication_connection->conn()->set_variable("default_transaction_isolation", "'repeatable read'");
    auto ntx = std::make_shared<pqxx::nontransaction>(*replication_connection->conn());

    std::string snapshot_name, start_lsn;

    auto initial_sync = [&]()
    {
        createReplicationSlot(ntx, start_lsn, snapshot_name);
        loadFromSnapshot(snapshot_name);
    };

    /// Replication slot should be deleted with drop table only and created only once, reused after detach.
    if (!isReplicationSlotExist(ntx, replication_slot))
    {
        initial_sync();
    }
    else if (!Poco::File(metadata_path).exists() || new_publication_created)
    {
        /// In case of some failure, the following cases are possible (since publication and replication slot are reused):
        /// 1. If replication slot exists and metadata file (where last synced version is written) does not exist, it is not ok.
        /// 2. If created a new publication and replication slot existed before it was created, it is not ok.
        dropReplicationSlot(ntx, replication_slot);
        initial_sync();
    }
    else
    {
        for (const auto & [table_name, storage] : storages)
        {
            nested_storages[table_name] = storage->getNested();
            storage->setNestedLoaded();
        }
    }

    ntx->commit();

    consumer = std::make_shared<PostgreSQLReplicaConsumer>(
            context,
            std::move(connection),
            replication_slot,
            publication_name,
            metadata_path,
            start_lsn,
            max_block_size,
            nested_storages);

    consumer->startSynchronization();

    replication_connection->conn()->close();
}


void PostgreSQLReplicationHandler::loadFromSnapshot(std::string & snapshot_name)
{
    for (const auto & storage_data : storages)
    {
        try
        {
            auto tx = std::make_shared<pqxx::work>(*connection->conn());

            /// Specific isolation level is required to read from snapshot.
            tx->set_variable("transaction_isolation", "'repeatable read'");

            std::string query_str = fmt::format("SET TRANSACTION SNAPSHOT '{}'", snapshot_name);
            tx->exec(query_str);

            storage_data.second->createNestedIfNeeded([&]() { return fetchTableStructure(tx, storage_data.first); });
            auto nested_storage = storage_data.second->getNested();

            /// Load from snapshot, which will show table state before creation of replication slot.
            /// Already connected to needed database, no need to add it to query.
            query_str = fmt::format("SELECT * FROM {}", storage_data.first);

            auto insert = std::make_shared<ASTInsertQuery>();
            insert->table_id = nested_storage->getStorageID();
            auto insert_context = storage_data.second->makeNestedTableContext();

            InterpreterInsertQuery interpreter(insert, insert_context);
            auto block_io = interpreter.execute();

            const StorageInMemoryMetadata & storage_metadata = nested_storage->getInMemoryMetadata();
            auto sample_block = storage_metadata.getSampleBlockNonMaterialized();

            PostgreSQLBlockInputStream<pqxx::work> input(tx, query_str, sample_block, DEFAULT_BLOCK_SIZE);

            copyData(input, *block_io.out);

            storage_data.second->setNestedLoaded();
            nested_storages[storage_data.first] = nested_storage;
        }
        catch (Exception & e)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
            e.addMessage("while initial data synchronization for table {}", storage_data.first);
            throw;
        }
    }

    LOG_DEBUG(log, "Table dump end");
}


bool PostgreSQLReplicationHandler::isPublicationExist(std::shared_ptr<pqxx::work> tx)
{
    std::string query_str = fmt::format("SELECT exists (SELECT 1 FROM pg_publication WHERE pubname = '{}')", publication_name);
    pqxx::result result{tx->exec(query_str)};
    assert(!result.empty());
    bool publication_exists = (result[0][0].as<std::string>() == "t");

    if (publication_exists)
        LOG_INFO(log, "Publication {} already exists. Using existing version", publication_name);

    return publication_exists;
}


void PostgreSQLReplicationHandler::createPublicationIfNeeded(
        PostgreSQLConnection::ConnectionPtr connection_)
{
    if (new_publication_created)
        return;

    auto tx = std::make_shared<pqxx::work>(*connection_);

    if (!isPublicationExist(tx))
    {
        if (tables_list.empty())
        {
            for (const auto & storage_data : storages)
            {
                if (!tables_list.empty())
                    tables_list += ", ";
                tables_list += storage_data.first;
            }
        }

        /// 'ONLY' means just a table, without descendants.
        std::string query_str = fmt::format("CREATE PUBLICATION {} FOR TABLE ONLY {}", publication_name, tables_list);
        try
        {
            tx->exec(query_str);
            new_publication_created = true;
            LOG_TRACE(log, "Created publication {} with tables list: {}", publication_name, tables_list);
        }
        catch (Exception & e)
        {
            e.addMessage("while creating pg_publication");
            throw;
        }
    }

    tx->commit();
}


bool PostgreSQLReplicationHandler::isReplicationSlotExist(NontransactionPtr ntx, std::string & slot_name)
{
    std::string query_str = fmt::format("SELECT active, restart_lsn FROM pg_replication_slots WHERE slot_name = '{}'", slot_name);
    pqxx::result result{ntx->exec(query_str)};

    /// Replication slot does not exist
    if (result.empty())
        return false;

    bool is_active = result[0][0].as<bool>();
    LOG_TRACE(log, "Replication slot {} already exists (active: {}). Restart lsn position is {}",
            slot_name, is_active, result[0][0].as<std::string>());

    return true;
}


void PostgreSQLReplicationHandler::createReplicationSlot(NontransactionPtr ntx, std::string & start_lsn, std::string & snapshot_name)
{
    std::string query_str = fmt::format("CREATE_REPLICATION_SLOT {} LOGICAL pgoutput EXPORT_SNAPSHOT", replication_slot);
    try
    {
        pqxx::result result{ntx->exec(query_str)};
        start_lsn = result[0][1].as<std::string>();
        snapshot_name = result[0][2].as<std::string>();
        LOG_TRACE(log, "Created replication slot: {}, start lsn: {}, snapshot: {}",
                replication_slot, start_lsn, snapshot_name);
    }
    catch (Exception & e)
    {
        e.addMessage("while creating PostgreSQL replication slot {}", replication_slot);
        throw;
    }
}


void PostgreSQLReplicationHandler::dropReplicationSlot(NontransactionPtr ntx, std::string & slot_name)
{
    std::string query_str = fmt::format("SELECT pg_drop_replication_slot('{}')", slot_name);
    ntx->exec(query_str);
    LOG_TRACE(log, "Dropped replication slot {}", slot_name);
}


void PostgreSQLReplicationHandler::dropPublication(NontransactionPtr ntx)
{
    std::string query_str = fmt::format("DROP PUBLICATION IF EXISTS {}", publication_name);
    ntx->exec(query_str);
}


void PostgreSQLReplicationHandler::shutdownFinal()
{
    if (Poco::File(metadata_path).exists())
        Poco::File(metadata_path).remove();

    connection = std::make_shared<PostgreSQLConnection>(connection_str);
    auto tx = std::make_shared<pqxx::nontransaction>(*connection->conn());

    dropPublication(tx);
    if (isReplicationSlotExist(tx, replication_slot))
        dropReplicationSlot(tx, replication_slot);

    tx->commit();
}


std::unordered_set<std::string> PostgreSQLReplicationHandler::fetchRequiredTables(PostgreSQLConnection::ConnectionPtr connection_)
{
    if (tables_list.empty())
    {
        return fetchPostgreSQLTablesList(connection_);
    }
    else
    {
        createPublicationIfNeeded(connection_);
        return fetchTablesFromPublication(connection_);
    }
}


std::unordered_set<std::string> PostgreSQLReplicationHandler::fetchTablesFromPublication(PostgreSQLConnection::ConnectionPtr connection_)
{
    std::string query = fmt::format("SELECT tablename FROM pg_publication_tables WHERE pubname = '{}'", publication_name);
    std::unordered_set<std::string> tables;
    pqxx::read_transaction tx(*connection_);

    for (auto table_name : tx.stream<std::string>(query))
        tables.insert(std::get<0>(table_name));

    return tables;
}


PostgreSQLTableStructure PostgreSQLReplicationHandler::fetchTableStructure(
        std::shared_ptr<pqxx::work> tx, const std::string & table_name)
{
    auto use_nulls = context->getSettingsRef().external_table_functions_use_nulls;
    return fetchPostgreSQLTableStructure(tx, table_name, use_nulls, true);
}

}
