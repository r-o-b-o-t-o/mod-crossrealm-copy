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

#include "Chat.h"
#include "ChatCommand.h"
#include "CrossRealmCopyMgr.h"
#include "ScriptMgr.h"

#ifndef _WIN32
#include <csignal>
#endif

using namespace Acore::ChatCommands;

class crossrealm_copy_commandscript : public CommandScript
{
public:
    crossrealm_copy_commandscript() : CommandScript("crossrealm_copy_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "character copy", HandleCharacterCopyCommand, SEC_PLAYER, Console::No }
        };

        return commandTable;
    }

    static bool HandleCharacterCopyCommand(ChatHandler* handler, std::string sourceName)
    {
        return sCrossRealmCopyMgr->HandleCopyCommand(handler, sourceName);
    }
};

class crossrealm_copy_worldscript : public WorldScript
{
public:
    crossrealm_copy_worldscript() : WorldScript("crossrealm_copy_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sCrossRealmCopyMgr->LoadConfig();
    }

    void OnStartup() override
    {
#ifndef _WIN32
        // A MySQL connection dying mid-write must never take the worldserver down
        // with an unhandled SIGPIPE; with SIG_IGN the client library gets a regular
        // EPIPE error instead and the copy fails gracefully.
        signal(SIGPIPE, SIG_IGN);
#endif
    }

    void OnUpdate(uint32 diff) override
    {
        sCrossRealmCopyMgr->Update(diff);
    }
};

void AddCrossRealmCopyScripts()
{
    new crossrealm_copy_commandscript();
    new crossrealm_copy_worldscript();
}
