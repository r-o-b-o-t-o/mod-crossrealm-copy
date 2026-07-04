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

#include "CrossRealmCopyDefs.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "StringFormat.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace CrossRealmCopy
{
    std::vector<CopyTable> const& GetCopyTables()
    {
        using Rule = ColumnRuleKind;

        static std::vector<CopyTable> const tables =
        {
            // Wipe-only tables: rows of the target character that would otherwise be
            // orphaned. Sub-select based clauses must come before the referenced table.
            { "item_soulbound_trade_data", "itemGuid IN (SELECT guid FROM item_instance WHERE owner_guid = {})", false, {} },
            { "item_loot_storage", "containerGUID IN (SELECT guid FROM item_instance WHERE owner_guid = {})", false, {} },
            { "character_instance", "guid = {}", false, {} },
            { "character_social", "guid = {}", false, {} },
            { "character_achievement_offline_updates", "guid = {}", false, {} },
            { "lfg_data", "guid = {}", false, {} },
            { "petition", "ownerguid = {}", false, {} },
            { "petition_sign", "ownerguid = {}", false, {} },

            // Pet data. The child tables are keyed by pet number and must be handled
            // before character_pet is wiped, because their WHERE clause resolves the
            // owned pet numbers through character_pet.
            { "pet_spell", "guid IN (SELECT id FROM character_pet WHERE owner = {})", true,
                { { "guid", Rule::PetRef } } },
            { "pet_aura", "guid IN (SELECT id FROM character_pet WHERE owner = {})", true,
                { { "guid", Rule::PetRef }, { "casterGuid", Rule::SelfGuidOrKeep } } },
            { "pet_spell_cooldown", "guid IN (SELECT id FROM character_pet WHERE owner = {})", true,
                { { "guid", Rule::PetRef } } },
            { "character_pet_declinedname", "owner = {}", true,
                { { "id", Rule::PetRef }, { "owner", Rule::TargetCharGuid } } },
            { "character_pet", "owner = {}", true,
                { { "id", Rule::NewPetNumber }, { "owner", Rule::TargetCharGuid } } },

            // Items. Auction-held items stay out on both sides: on the source they
            // belong to a live auction of that realm, on the target wiping them would
            // corrupt running auctions.
            { "item_instance", "owner_guid = {} AND guid NOT IN (SELECT itemguid FROM auctionhouse)", true,
                { { "guid", Rule::NewItemGuid }, { "owner_guid", Rule::TargetCharGuid },
                  { "creatorGuid", Rule::SelfGuidOrKeep }, { "giftCreatorGuid", Rule::SelfGuidOrKeep } } },
            { "character_inventory", "guid = {}", true,
                { { "guid", Rule::TargetCharGuid }, { "bag", Rule::ItemRefOrZero }, { "item", Rule::ItemRef } } },
            { "character_gifts", "guid = {}", true,
                { { "guid", Rule::TargetCharGuid }, { "item_guid", Rule::ItemRef } } },
            { "item_refund_instance", "player_guid = {}", true,
                { { "player_guid", Rule::TargetCharGuid }, { "item_guid", Rule::ItemRef } } },
            { "character_equipmentsets", "guid = {}", true,
                { { "guid", Rule::TargetCharGuid }, { "setguid", Rule::NewEquipmentSetGuid },
                  { "item0", Rule::ItemRefOrZero }, { "item1", Rule::ItemRefOrZero }, { "item2", Rule::ItemRefOrZero },
                  { "item3", Rule::ItemRefOrZero }, { "item4", Rule::ItemRefOrZero }, { "item5", Rule::ItemRefOrZero },
                  { "item6", Rule::ItemRefOrZero }, { "item7", Rule::ItemRefOrZero }, { "item8", Rule::ItemRefOrZero },
                  { "item9", Rule::ItemRefOrZero }, { "item10", Rule::ItemRefOrZero }, { "item11", Rule::ItemRefOrZero },
                  { "item12", Rule::ItemRefOrZero }, { "item13", Rule::ItemRefOrZero }, { "item14", Rule::ItemRefOrZero },
                  { "item15", Rule::ItemRefOrZero }, { "item16", Rule::ItemRefOrZero }, { "item17", Rule::ItemRefOrZero },
                  { "item18", Rule::ItemRefOrZero } } },

            // Mail. Mailed items are included in the item_instance copy because their
            // owner_guid is the receiver.
            { "mail", "receiver = {}", true,
                { { "id", Rule::NewMailId }, { "receiver", Rule::TargetCharGuid }, { "sender", Rule::SelfGuidOrKeep } } },
            { "mail_items", "receiver = {}", true,
                { { "mail_id", Rule::MailRef }, { "item_guid", Rule::ItemRef }, { "receiver", Rule::TargetCharGuid } } },

            // Plain per-character tables.
            { "character_account_data", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_achievement", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_achievement_progress", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_action", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_arena_stats", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_aura", "guid = {}", true,
                { { "guid", Rule::TargetCharGuid }, { "casterGuid", Rule::SelfGuidOrKeep },
                  { "itemGuid", Rule::ItemRefFullOrZero } } },
            { "character_battleground_random", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_brew_of_the_month", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_entry_point", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_glyphs", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_homebind", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_queststatus", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_queststatus_daily", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_queststatus_weekly", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_queststatus_monthly", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_queststatus_seasonal", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_queststatus_rewarded", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_reputation", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_settings", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_skills", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_spell", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_spell_cooldown", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_stats", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },
            { "character_talent", "guid = {}", true, { { "guid", Rule::TargetCharGuid } } },

            // The characters row itself. The target keeps its guid, account and name;
            // volatile realm-local fields are reset.
            { "characters", "guid = {}", true,
                { { "guid", Rule::TargetCharGuid }, { "account", Rule::TargetAccount }, { "name", Rule::TargetName },
                  { "online", Rule::ForceZero }, { "instance_id", Rule::ForceZero },
                  { "trans_x", Rule::ForceZero }, { "trans_y", Rule::ForceZero }, { "trans_z", Rule::ForceZero },
                  { "trans_o", Rule::ForceZero }, { "transguid", Rule::ForceZero },
                  { "deleteInfos_Account", Rule::ForceNull }, { "deleteInfos_Name", Rule::ForceNull },
                  { "deleteDate", Rule::ForceNull } } }
        };

        return tables;
    }

    std::string EscapeSqlLiteral(std::string_view value)
    {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value)
        {
            switch (c)
            {
                case '\0': out += "\\0"; break;
                case '\'': out += "\\'"; break;
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\x1a': out += "\\Z"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    bool IsSafeIdentifier(std::string_view identifier)
    {
        if (identifier.empty() || identifier.size() > 64)
            return false;

        for (char c : identifier)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$')
                return false;

        return true;
    }
}

namespace
{
    using namespace CrossRealmCopy;

    // Rows per generated INSERT statement.
    constexpr std::size_t INSERT_CHUNK_SIZE = 200;

    bool StrEqualI(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;

        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;

        return true;
    }

    ColumnRule const* FindRule(CopyTable const& spec, std::string_view column)
    {
        for (ColumnRule const& rule : spec.rules)
            if (StrEqualI(rule.column, column))
                return &rule;

        return nullptr;
    }

    bool ParseUInt64(DbValue const& value, uint64& out)
    {
        if (value.isNull || value.data.empty())
            return false;

        char* end = nullptr;
        out = std::strtoull(value.data.c_str(), &end, 10);
        return end && *end == '\0';
    }

    bool ContainsColumn(std::vector<std::string> const& columns, std::string_view column)
    {
        for (std::string const& candidate : columns)
            if (StrEqualI(candidate, column))
                return true;

        return false;
    }

    void AppendVerbatim(DbValue const& value, std::string& out)
    {
        if (value.isNull)
        {
            out += "NULL";
            return;
        }

        out += '\'';
        out += EscapeSqlLiteral(value.data);
        out += '\'';
    }

    bool AppendRemapped(std::unordered_map<uint64, uint64> const& map, DbValue const& value, bool zeroWhenUnknown,
        std::string& out)
    {
        uint64 old = 0;
        if (!ParseUInt64(value, old) || !old)
        {
            if (!zeroWhenUnknown)
                return false;

            out += '0';
            return true;
        }

        auto itr = map.find(old);
        if (itr == map.end())
        {
            if (!zeroWhenUnknown)
                return false;

            out += '0';
            return true;
        }

        out += Acore::StringFormat("{}", itr->second);
        return true;
    }

    // Renders one column value. Returns false when the whole row must be dropped
    // (a required reference points at a row that was not copied).
    bool RenderValue(ColumnRule const* rule, DbValue const& value, GuidRemaps const& remaps, std::string& out)
    {
        if (!rule)
        {
            AppendVerbatim(value, out);
            return true;
        }

        switch (rule->kind)
        {
            case ColumnRuleKind::TargetCharGuid:
                out += Acore::StringFormat("{}", remaps.targetLow);
                return true;
            case ColumnRuleKind::TargetAccount:
                out += Acore::StringFormat("{}", remaps.targetAccount);
                return true;
            case ColumnRuleKind::TargetName:
                out += '\'';
                out += EscapeSqlLiteral(remaps.targetName);
                out += '\'';
                return true;
            case ColumnRuleKind::ForceZero:
                out += '0';
                return true;
            case ColumnRuleKind::ForceNull:
                out += "NULL";
                return true;
            case ColumnRuleKind::NewItemGuid:
                return AppendRemapped(remaps.items, value, false, out);
            case ColumnRuleKind::ItemRef:
                return AppendRemapped(remaps.items, value, false, out);
            case ColumnRuleKind::ItemRefOrZero:
                return AppendRemapped(remaps.items, value, true, out);
            case ColumnRuleKind::ItemRefFullOrZero:
            {
                uint64 full = 0;
                if (!ParseUInt64(value, full) || !full)
                {
                    out += '0';
                    return true;
                }

                auto itr = remaps.items.find(full & 0xFFFFFFFF);
                if (itr == remaps.items.end())
                {
                    out += '0';
                    return true;
                }

                out += Acore::StringFormat("{}", (full & ~uint64(0xFFFFFFFF)) | itr->second);
                return true;
            }
            case ColumnRuleKind::NewPetNumber:
            case ColumnRuleKind::PetRef:
                return AppendRemapped(remaps.pets, value, false, out);
            case ColumnRuleKind::NewMailId:
            case ColumnRuleKind::MailRef:
                return AppendRemapped(remaps.mails, value, false, out);
            case ColumnRuleKind::NewEquipmentSetGuid:
                return AppendRemapped(remaps.equipmentSets, value, false, out);
            case ColumnRuleKind::SelfGuidOrKeep:
            {
                uint64 old = 0;
                if (ParseUInt64(value, old) && old == remaps.sourceLow)
                    out += Acore::StringFormat("{}", remaps.targetLow);
                else
                    AppendVerbatim(value, out);
                return true;
            }
            default:
                AppendVerbatim(value, out);
                return true;
        }
    }

    void AllocateForTable(CopyTable const& spec, TableSnapshot const& table, std::size_t columnIndex,
        ColumnRuleKind kind, GuidRemaps& remaps)
    {
        for (auto const& row : table.rows)
        {
            uint64 old = 0;
            if (!ParseUInt64(row[columnIndex], old) || !old)
                continue;

            switch (kind)
            {
                case ColumnRuleKind::NewItemGuid:
                    if (remaps.items.find(old) == remaps.items.end())
                        remaps.items[old] = sObjectMgr->GetGenerator<HighGuid::Item>().Generate();
                    break;
                case ColumnRuleKind::NewPetNumber:
                    if (remaps.pets.find(old) == remaps.pets.end())
                        remaps.pets[old] = sObjectMgr->GeneratePetNumber();
                    break;
                case ColumnRuleKind::NewMailId:
                    if (remaps.mails.find(old) == remaps.mails.end())
                        remaps.mails[old] = sObjectMgr->GenerateMailID();
                    break;
                case ColumnRuleKind::NewEquipmentSetGuid:
                    if (remaps.equipmentSets.find(old) == remaps.equipmentSets.end())
                        remaps.equipmentSets[old] = sObjectMgr->GenerateEquipmentSetGuid();
                    break;
                default:
                    break;
            }
        }
    }
}

namespace CrossRealmCopy
{
    void AllocateNewIds(CharacterSnapshot const& snapshot, GuidRemaps& remaps)
    {
        std::vector<CopyTable> const& specs = GetCopyTables();

        for (TableSnapshot const& table : snapshot.tables)
        {
            CopyTable const& spec = specs[table.specIndex];
            for (ColumnRule const& rule : spec.rules)
            {
                if (rule.kind != ColumnRuleKind::NewItemGuid && rule.kind != ColumnRuleKind::NewPetNumber &&
                    rule.kind != ColumnRuleKind::NewMailId && rule.kind != ColumnRuleKind::NewEquipmentSetGuid)
                    continue;

                for (std::size_t i = 0; i < table.columns.size(); ++i)
                    if (StrEqualI(table.columns[i], rule.column))
                        AllocateForTable(spec, table, i, rule.kind, remaps);
            }
        }
    }

    bool BuildApplyTransaction(CharacterSnapshot const& snapshot, GuidRemaps const& remaps,
        CharacterDatabaseTransaction trans, std::string& error)
    {
        std::vector<CopyTable> const& specs = GetCopyTables();

        if (snapshot.targetColumns.find("characters") == snapshot.targetColumns.end())
        {
            error = "the characters table layout of this realm could not be read";
            return false;
        }

        // Wipe the target character's rows, in spec order.
        for (CopyTable const& spec : specs)
        {
            if (snapshot.targetColumns.find(spec.table) == snapshot.targetColumns.end())
            {
                LOG_WARN("module", "[CrossRealmCopy] Table {} does not exist on this realm, skipping wipe", spec.table);
                continue;
            }

            trans->Append(Acore::StringFormat("DELETE FROM `{}` WHERE {}", spec.table,
                Acore::StringFormat(spec.where, remaps.targetLow)));
        }

        // Copy the snapshot rows.
        bool wroteCharactersRow = false;
        for (TableSnapshot const& table : snapshot.tables)
        {
            CopyTable const& spec = specs[table.specIndex];
            if (table.rows.empty())
                continue;

            auto targetItr = snapshot.targetColumns.find(spec.table);
            if (targetItr == snapshot.targetColumns.end())
                continue;

            // Only copy columns both schemas share, so realm-specific custom columns
            // never break the whole transaction.
            std::vector<std::size_t> plan;
            std::vector<ColumnRule const*> planRules;
            std::string columnList;
            for (std::size_t i = 0; i < table.columns.size(); ++i)
            {
                std::string const& column = table.columns[i];
                if (!IsSafeIdentifier(column))
                {
                    LOG_WARN("module", "[CrossRealmCopy] Ignoring unsafe column name {}.{}", spec.table, column);
                    continue;
                }

                if (!ContainsColumn(targetItr->second, column))
                {
                    LOG_WARN("module", "[CrossRealmCopy] Source column {}.{} does not exist on this realm, skipping it",
                        spec.table, column);
                    continue;
                }

                if (!columnList.empty())
                    columnList += ", ";
                columnList += '`';
                columnList += column;
                columnList += '`';
                plan.push_back(i);
                planRules.push_back(FindRule(spec, column));
            }

            if (plan.empty())
            {
                LOG_WARN("module", "[CrossRealmCopy] No copyable columns for table {}, skipping it", spec.table);
                continue;
            }

            std::string insertHead = Acore::StringFormat("INSERT INTO `{}` ({}) VALUES ", spec.table, columnList);
            std::string statement;
            std::size_t rowsInChunk = 0;
            uint32 droppedRows = 0;

            for (auto const& row : table.rows)
            {
                std::string rendered = "(";
                bool keepRow = true;
                for (std::size_t p = 0; p < plan.size(); ++p)
                {
                    if (p)
                        rendered += ", ";

                    if (!RenderValue(planRules[p], row[plan[p]], remaps, rendered))
                    {
                        keepRow = false;
                        break;
                    }
                }

                if (!keepRow)
                {
                    ++droppedRows;
                    continue;
                }

                rendered += ')';
                statement += statement.empty() ? insertHead : std::string(", ");
                statement += rendered;

                if (++rowsInChunk >= INSERT_CHUNK_SIZE)
                {
                    trans->Append(statement);
                    statement.clear();
                    rowsInChunk = 0;
                }
            }

            if (!statement.empty())
                trans->Append(statement);

            if (droppedRows)
                LOG_WARN("module", "[CrossRealmCopy] Dropped {} row(s) of {} because of unresolvable references",
                    droppedRows, spec.table);

            if (!strcmp(spec.table, "characters") && (table.rows.size() - droppedRows) == 1)
                wroteCharactersRow = true;
        }

        if (!wroteCharactersRow)
        {
            error = "the source characters row could not be prepared for this realm";
            return false;
        }

        return true;
    }
}
