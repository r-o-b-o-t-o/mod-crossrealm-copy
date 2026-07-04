/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "SourceRealmDb.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "StringFormat.h"
#include <cstdlib>

#include "Database/MySQLWorkaround.h"

namespace
{
    using namespace CrossRealmCopy;

    // MySQL server error: table does not exist. Kept as a literal so we do not
    // depend on mysqld_error.h being shipped with every client library.
    constexpr unsigned int ERRNO_NO_SUCH_TABLE = 1146;

    // Fresh connection to the source realm's database, closed on destruction.
    // Never shared between threads or reused across copies.
    class SourceConnection
    {
    public:
        explicit SourceConnection(SourceDbConfig const& config) : _config(config) { }

        SourceConnection(SourceConnection const&) = delete;
        SourceConnection& operator=(SourceConnection const&) = delete;

        ~SourceConnection()
        {
            if (_mysql)
                mysql_close(_mysql);

            // Release the per-thread state of the client library. Worker threads are
            // not the ones the core initialized, so this must be balanced here.
            mysql_thread_end();
        }

        bool Connect(std::string& error)
        {
            _mysql = mysql_init(nullptr);
            if (!_mysql)
            {
                error = "could not initialize the MySQL client handle";
                return false;
            }

            // A stuck source database must never block a copy (or the worker thread)
            // forever: bound every network operation.
            unsigned int connectTimeout = _config.connectTimeoutSecs;
            unsigned int readTimeout = _config.readTimeoutSecs;
            unsigned int writeTimeout = _config.readTimeoutSecs;
            mysql_options(_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
            mysql_options(_mysql, MYSQL_OPT_READ_TIMEOUT, &readTimeout);
            mysql_options(_mysql, MYSQL_OPT_WRITE_TIMEOUT, &writeTimeout);
            mysql_options(_mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");

            if (!mysql_real_connect(_mysql, _config.host.c_str(), _config.user.c_str(), _config.password.c_str(),
                _config.database.c_str(), _config.port, nullptr, 0))
            {
                error = Acore::StringFormat("could not connect to the source database: {}", mysql_error(_mysql));
                return false;
            }

            return true;
        }

        bool Execute(std::string const& sql, std::string& error)
        {
            if (mysql_real_query(_mysql, sql.c_str(), sql.length()))
            {
                error = Acore::StringFormat("query failed ({}): {}", mysql_errno(_mysql), mysql_error(_mysql));
                return false;
            }

            return true;
        }

        // Runs a SELECT and stores columns and rows into the snapshot table.
        // A missing table on the source realm is not an error: the table snapshot
        // simply stays empty (the target rows still get wiped).
        bool FetchTable(CopyTable const& spec, uint32 whereKey, TableSnapshot& out, std::string& error)
        {
            std::string sql = Acore::StringFormat("SELECT * FROM `{}` WHERE {}", spec.table,
                Acore::StringFormat(spec.where, whereKey));

            if (mysql_real_query(_mysql, sql.c_str(), sql.length()))
            {
                if (mysql_errno(_mysql) == ERRNO_NO_SUCH_TABLE)
                {
                    if (spec.optional)
                        LOG_DEBUG("module", "[CrossRealmCopy] Optional table {} does not exist on the source realm, "
                            "skipping it", spec.table);
                    else
                        LOG_WARN("module", "[CrossRealmCopy] Table {} does not exist on the source realm, skipping it",
                            spec.table);
                    return true;
                }

                error = Acore::StringFormat("could not read table {}: {}", spec.table, mysql_error(_mysql));
                return false;
            }

            MYSQL_RES* result = mysql_store_result(_mysql);
            if (!result)
            {
                if (mysql_errno(_mysql))
                {
                    error = Acore::StringFormat("could not fetch table {}: {}", spec.table, mysql_error(_mysql));
                    return false;
                }

                return true;
            }

            unsigned int fieldCount = mysql_num_fields(result);
            MYSQL_FIELD* fields = mysql_fetch_fields(result);
            out.columns.reserve(fieldCount);
            for (unsigned int i = 0; i < fieldCount; ++i)
                out.columns.emplace_back(fields[i].name ? fields[i].name : "");

            while (MYSQL_ROW row = mysql_fetch_row(result))
            {
                unsigned long* lengths = mysql_fetch_lengths(result);
                std::vector<DbValue> values(fieldCount);
                for (unsigned int i = 0; i < fieldCount; ++i)
                {
                    if (!row[i])
                        continue;

                    values[i].isNull = false;
                    values[i].data.assign(row[i], lengths[i]);
                }

                out.rows.push_back(std::move(values));
            }

            mysql_free_result(result);
            return true;
        }

        MYSQL* Handle() { return _mysql; }

    private:
        SourceDbConfig const& _config;
        MYSQL* _mysql = nullptr;
    };

    uint64 FieldToUInt64(char const* value)
    {
        return value ? std::strtoull(value, nullptr, 10) : 0;
    }
}

namespace CrossRealmCopy
{
    SourceCharacter LookupSourceCharacter(SourceDbConfig config, std::string characterName)
    {
        SourceCharacter out;

        SourceConnection connection(config);
        if (!connection.Connect(out.error))
            return out;

        // characters.name uses a binary collation, so compare case-insensitively.
        std::string sql = Acore::StringFormat(
            "SELECT guid, account, name, race, class, level, online FROM characters "
            "WHERE UPPER(name) = UPPER('{}') LIMIT 1",
            EscapeSqlLiteral(characterName));

        if (!connection.Execute(sql, out.error))
            return out;

        MYSQL_RES* result = mysql_store_result(connection.Handle());
        if (!result)
        {
            out.error = Acore::StringFormat("could not fetch the character lookup result: {}",
                mysql_error(connection.Handle()));
            return out;
        }

        if (MYSQL_ROW row = mysql_fetch_row(result))
        {
            out.found = true;
            out.guid = static_cast<uint32>(FieldToUInt64(row[0]));
            out.account = static_cast<uint32>(FieldToUInt64(row[1]));
            out.name = row[2] ? row[2] : "";
            out.race = static_cast<uint8>(FieldToUInt64(row[3]));
            out.charClass = static_cast<uint8>(FieldToUInt64(row[4]));
            out.level = static_cast<uint8>(FieldToUInt64(row[5]));
            out.online = FieldToUInt64(row[6]) != 0;
        }

        mysql_free_result(result);
        return out;
    }

    CharacterSnapshot ReadSourceCharacter(SourceDbConfig config, uint32 sourceGuid)
    {
        CharacterSnapshot snapshot;
        snapshot.sourceGuid = sourceGuid;

        std::vector<CopyTable> const& specs = GetCopyTables();

        // Column layout of this realm's tables, through the core pool (synchronous
        // queries are safe from a worker thread). Missing tables simply stay out of
        // the map and are skipped later.
        for (CopyTable const& spec : specs)
        {
            QueryResult result = CharacterDatabase.Query(
                Acore::StringFormat("SHOW COLUMNS FROM `{}`", spec.table));
            if (!result)
                continue;

            std::vector<std::string> columns;
            do
                columns.push_back(result->Fetch()[0].Get<std::string>());
            while (result->NextRow());

            snapshot.targetColumns.emplace(spec.table, std::move(columns));
        }

        SourceConnection connection(config);
        if (!connection.Connect(snapshot.error))
            return snapshot;

        // One consistent view over all the SELECTs, so a character saved by the
        // source realm mid-copy cannot tear the snapshot.
        if (!connection.Execute("START TRANSACTION WITH CONSISTENT SNAPSHOT", snapshot.error))
            return snapshot;

        // The character could have been deleted or logged in since the lookup. The
        // account id keys the reads of account-wide tables below.
        {
            std::string sql = Acore::StringFormat("SELECT online, account FROM characters WHERE guid = {}", sourceGuid);
            if (!connection.Execute(sql, snapshot.error))
                return snapshot;

            MYSQL_RES* result = mysql_store_result(connection.Handle());
            if (!result)
            {
                snapshot.error = Acore::StringFormat("could not fetch the source character: {}",
                    mysql_error(connection.Handle()));
                return snapshot;
            }

            MYSQL_ROW row = mysql_fetch_row(result);
            bool online = row && FieldToUInt64(row[0]) != 0;
            bool found = row != nullptr;
            if (row)
                snapshot.sourceAccount = static_cast<uint32>(FieldToUInt64(row[1]));
            mysql_free_result(result);

            if (!found)
            {
                snapshot.error = "the source character no longer exists";
                return snapshot;
            }

            if (online && !config.allowOnlineSource)
            {
                snapshot.error = "the source character is currently online on the source realm";
                return snapshot;
            }
        }

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            if (!specs[i].copyRows)
                continue;

            uint32 whereKey = specs[i].whereKey == WhereKey::AccountId ? snapshot.sourceAccount : sourceGuid;
            TableSnapshot table;
            table.specIndex = i;
            if (!connection.FetchTable(specs[i], whereKey, table, snapshot.error))
                return snapshot;

            snapshot.tables.push_back(std::move(table));
        }

        return snapshot;
    }
}
