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

#ifndef MOD_CROSSREALM_COPY_MGR_H
#define MOD_CROSSREALM_COPY_MGR_H

#include "AsyncCallbackProcessor.h"
#include "CrossRealmCopyDefs.h"
#include "ObjectGuid.h"
#include "QueryCallback.h"
#include "Transaction.h"
#include <deque>
#include <future>
#include <memory>
#include <optional>

class ChatHandler;

namespace CrossRealmCopy
{
    enum class CopyState : uint8
    {
        Queued,         // waiting for its turn
        LookingUp,      // source character being looked up on the source realm (worker thread)
        Warning,        // player warned, waiting for the kick deadline
        Banning,        // temporary character ban being committed
        WaitingLogout,  // waiting for the player to leave the world and the logout save to land
        Reading,        // source character being snapshotted (worker thread)
        Committing      // transaction with the copied data being committed
    };

    struct CopyRequest
    {
        uint32 id = 0;
        // The player is never stored as a pointer: requests outlive world ticks and the
        // player object can be freed at any time. Resolved on use via ObjectAccessor.
        ObjectGuid targetGuid;
        uint32 accountId = 0;
        std::string targetName;
        std::string sourceName;
        uint8 race = 0;
        uint8 charClass = 0;
        bool gmBypass = false;

        CopyState state = CopyState::Queued;
        uint32 stateTimer = 0;              // remaining ms before the timed action / timeout of the state
        uint32 onlinePollTimer = 0;
        bool banned = false;
        bool finished = false;
        bool onlineCheckPending = false;
        bool logoutSaveConfirmed = false;
        bool commitDone = false;
        bool commitSuccess = false;

        SourceCharacter source;
        // Appearance ids merged into custom_unlocked_appearances, kept to refresh
        // mod-transmog's in-memory collection cache after the commit succeeds.
        std::vector<uint32> unlockedAppearances;
        std::future<SourceCharacter> lookupFuture;
        std::future<CharacterSnapshot> readFuture;
        std::optional<TransactionCallback> commitCallback;
    };

    class Mgr
    {
    public:
        static Mgr* instance();

        void LoadConfig();
        void Update(uint32 diff);

        // Handles ".character copy <name>". Always consumes the command.
        bool HandleCopyCommand(ChatHandler* handler, std::string const& sourceName);

    private:
        Mgr() = default;

        void ProcessFront(uint32 diff);
        void StartLookup(CopyRequest& request);
        void OnLookupDone(std::shared_ptr<CopyRequest> const& request);
        void StartBan(std::shared_ptr<CopyRequest> const& request);
        void OnBanCommitted(std::shared_ptr<CopyRequest> const& request);
        void UpdateLogoutWait(std::shared_ptr<CopyRequest> const& request, uint32 diff);
        void StartRead(CopyRequest& request);
        void OnReadDone(std::shared_ptr<CopyRequest> const& request);
        void FinishRequest(std::shared_ptr<CopyRequest> const& request, bool success, std::string const& message);
        void NotifyRequester(CopyRequest const& request, std::string const& message) const;
        void AnnounceQueuePositions() const;
        void SweepAbandonedJobs();

        bool _enabled = false;
        SourceDbConfig _dbConfig;
        bool _requireSameAccount = true;
        uint32 _gmBypassSecurity = 3;
        uint32 _maxTargetLevel = 1;
        uint32 _kickDelayMs = 5000;
        uint32 _banDurationSecs = 300;
        uint32 _logoutTimeoutMs = 60000;
        uint32 _maxQueueSize = 10;

        uint32 _nextRequestId = 1;
        std::deque<std::shared_ptr<CopyRequest>> _queue;
        QueryCallbackProcessor _queryProcessor;
        // Timed-out worker jobs whose futures still need to be reaped once they end.
        std::vector<std::future<SourceCharacter>> _abandonedLookups;
        std::vector<std::future<CharacterSnapshot>> _abandonedReads;
    };
}

#define sCrossRealmCopyMgr CrossRealmCopy::Mgr::instance()

#endif // MOD_CROSSREALM_COPY_MGR_H
