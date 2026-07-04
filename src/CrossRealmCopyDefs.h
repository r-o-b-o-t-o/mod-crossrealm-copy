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

#ifndef MOD_CROSSREALM_COPY_DEFS_H
#define MOD_CROSSREALM_COPY_DEFS_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace CrossRealmCopy
{
    // Connection settings for the source realm's characters database.
    struct SourceDbConfig
    {
        std::string host;
        uint32 port = 3306;
        std::string user;
        std::string password;
        std::string database;
        uint32 connectTimeoutSecs = 10;
        uint32 readTimeoutSecs = 30;
        bool allowOnlineSource = false;
    };

    // How a column is transformed when its row is written to the target realm.
    // Columns without a rule are copied verbatim.
    enum class ColumnRuleKind : uint8
    {
        TargetCharGuid,         // replaced with the target character's low guid
        TargetAccount,          // replaced with the target character's account id
        TargetName,             // replaced with the target character's name
        ForceZero,
        ForceNull,
        NewItemGuid,            // item low guid: a new guid is allocated on this realm
        ItemRef,                // item low guid reference: remapped; row dropped when unknown
        ItemRefOrZero,          // item low guid reference: remapped; 0 when unknown
        ItemRefFullOrZero,      // packed full item guid (uint64): low part remapped; 0 when unknown
        NewPetNumber,           // pet number: a new number is allocated on this realm
        PetRef,                 // pet number reference: remapped; row dropped when unknown
        NewMailId,              // mail id: a new id is allocated on this realm
        MailRef,                // mail id reference: remapped; row dropped when unknown
        NewEquipmentSetGuid,    // equipment set guid: a new guid is allocated on this realm
        SelfGuidOrKeep          // replaced with the target's low guid when it equals the
                                // source character's low guid, kept verbatim otherwise
    };

    struct ColumnRule
    {
        char const* column;
        ColumnRuleKind kind;
    };

    // What the "{}" placeholder of a WHERE clause is replaced with.
    enum class WhereKey : uint8
    {
        CharacterGuid,      // character low guid (source character / target character)
        AccountId           // account id (source account / target account)
    };

    struct CopyTable
    {
        char const* table;
        // WHERE clause with "{}" as placeholder (see whereKey). Used both to select
        // rows on the source realm and to delete rows on the target realm.
        char const* where;
        // false: target rows are only wiped, nothing is copied from the source realm.
        bool copyRows;
        std::vector<ColumnRule> rules;
        WhereKey whereKey = WhereKey::CharacterGuid;
        // false: never delete target rows; the copied rows are merged in instead
        // (for account-wide tables whose existing data must survive the copy).
        bool wipe = true;
        // Use INSERT IGNORE so merged rows can collide with existing primary keys.
        bool insertIgnore = false;
        // Table belongs to another optional module: log its absence quietly.
        bool optional = false;
    };

    // Ordered list of tables handled by the copy. The order matters for the DELETE
    // statements: sub-select based WHERE clauses must run before the table they
    // reference is wiped.
    std::vector<CopyTable> const& GetCopyTables();

    struct DbValue
    {
        bool isNull = true;
        std::string data;
    };

    struct TableSnapshot
    {
        std::size_t specIndex = 0;
        std::vector<std::string> columns;
        std::vector<std::vector<DbValue>> rows;
    };

    // Result of the source character lookup performed when a request reaches the
    // front of the queue.
    struct SourceCharacter
    {
        bool found = false;
        uint32 guid = 0;
        uint32 account = 0;
        std::string name;
        uint8 race = 0;
        uint8 charClass = 0;
        uint8 level = 0;
        bool online = false;
        std::string error;      // non-empty when the source database could not be queried
    };

    // Full data snapshot of the source character plus the column layout of the
    // target realm's tables (used to only insert columns both schemas share).
    struct CharacterSnapshot
    {
        uint32 sourceGuid = 0;
        uint32 sourceAccount = 0;
        std::vector<TableSnapshot> tables;
        std::map<std::string, std::vector<std::string>> targetColumns;
        std::string error;      // non-empty when the snapshot failed
    };

    // New ids allocated on this realm for rows that carry globally unique keys.
    struct GuidRemaps
    {
        uint32 sourceLow = 0;
        uint32 targetLow = 0;
        uint32 targetAccount = 0;
        std::string targetName;
        std::unordered_map<uint64, uint64> items;
        std::unordered_map<uint64, uint64> pets;
        std::unordered_map<uint64, uint64> mails;
        std::unordered_map<uint64, uint64> equipmentSets;
    };

    // Escapes a string for embedding in a single-quoted SQL literal. Both realms use
    // utf8 connections, so escaping byte by byte is safe.
    std::string EscapeSqlLiteral(std::string_view value);

    // True when the identifier only contains [A-Za-z0-9_$] and can safely be quoted
    // with backticks.
    bool IsSafeIdentifier(std::string_view identifier);

    // Allocates new item guids / pet numbers / mail ids / equipment set guids for the
    // snapshot rows that need them. Must be called from the world thread (the guid
    // generators are not thread safe).
    void AllocateNewIds(CharacterSnapshot const& snapshot, GuidRemaps& remaps);

    // Appends all DELETE (wipe) and INSERT (copy) statements to the transaction.
    // Returns false and sets error when the snapshot cannot be applied.
    bool BuildApplyTransaction(CharacterSnapshot const& snapshot, GuidRemaps const& remaps,
        CharacterDatabaseTransaction trans, std::string& error);
}

#endif // MOD_CROSSREALM_COPY_DEFS_H
