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

#include "CrossRealmCopyMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "PetitionMgr.h"
#include "Player.h"
#include "SourceRealmDb.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <chrono>

namespace
{
    constexpr uint32 ONLINE_POLL_INTERVAL_MS = 1000;
    // Worker jobs are bounded by the MySQL connect/read timeouts; these are just a
    // safety margin on top so a request can never wedge the queue.
    constexpr uint32 LOOKUP_TIMEOUT_MS = 90 * 1000;
    constexpr uint32 READ_TIMEOUT_MS = 300 * 1000;

    bool IsFutureReady(std::future<CrossRealmCopy::SourceCharacter> const& future)
    {
        return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    bool IsFutureReady(std::future<CrossRealmCopy::CharacterSnapshot> const& future)
    {
        return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
}

namespace CrossRealmCopy
{
    Mgr* Mgr::instance()
    {
        static Mgr instance;
        return &instance;
    }

    void Mgr::LoadConfig()
    {
        _enabled = sConfigMgr->GetOption<bool>("CrossRealmCopy.Enabled", false);
        _requireSameAccount = sConfigMgr->GetOption<bool>("CrossRealmCopy.RequireSameAccount", true);
        _gmBypassSecurity = sConfigMgr->GetOption<uint32>("CrossRealmCopy.GMBypassSecurity", 3);
        _maxTargetLevel = sConfigMgr->GetOption<uint32>("CrossRealmCopy.MaxTargetLevel", 1);
        _kickDelayMs = sConfigMgr->GetOption<uint32>("CrossRealmCopy.KickDelay", 5) * 1000;
        _banDurationSecs = sConfigMgr->GetOption<uint32>("CrossRealmCopy.BanDuration", 300);
        _logoutTimeoutMs = sConfigMgr->GetOption<uint32>("CrossRealmCopy.LogoutTimeout", 60) * 1000;
        _maxQueueSize = sConfigMgr->GetOption<uint32>("CrossRealmCopy.MaxQueueSize", 10);

        _dbConfig.connectTimeoutSecs = sConfigMgr->GetOption<uint32>("CrossRealmCopy.ConnectTimeout", 10);
        _dbConfig.readTimeoutSecs = sConfigMgr->GetOption<uint32>("CrossRealmCopy.ReadTimeout", 30);
        _dbConfig.allowOnlineSource = sConfigMgr->GetOption<bool>("CrossRealmCopy.AllowOnlineSource", false);

        std::string info = sConfigMgr->GetOption<std::string>("CrossRealmCopy.SourceDatabaseInfo", "");
        std::vector<std::string_view> tokens = Acore::Tokenize(info, ';', true);
        if (tokens.size() == 5)
        {
            _dbConfig.host = tokens[0];
            _dbConfig.port = Acore::StringTo<uint32>(tokens[1]).value_or(3306);
            _dbConfig.user = tokens[2];
            _dbConfig.password = tokens[3];
            _dbConfig.database = tokens[4];
        }
        else if (_enabled)
        {
            LOG_ERROR("module", "[CrossRealmCopy] CrossRealmCopy.SourceDatabaseInfo is malformed "
                "(expected \"host;port;user;password;database\"), disabling the module");
            _enabled = false;
        }
    }

    void Mgr::Update(uint32 diff)
    {
        _queryProcessor.ProcessReadyCallbacks();
        SweepAbandonedJobs();

        if (!_queue.empty())
            ProcessFront(diff);
    }

    void Mgr::SweepAbandonedJobs()
    {
        std::erase_if(_abandonedLookups, [](std::future<SourceCharacter> const& future) { return IsFutureReady(future); });
        std::erase_if(_abandonedReads, [](std::future<CharacterSnapshot> const& future) { return IsFutureReady(future); });
    }

    void Mgr::ProcessFront(uint32 diff)
    {
        // Keep a strong reference: FinishRequest pops the queue.
        std::shared_ptr<CopyRequest> request = _queue.front();

        switch (request->state)
        {
            case CopyState::Queued:
                StartLookup(*request);
                break;
            case CopyState::LookingUp:
                if (IsFutureReady(request->lookupFuture))
                {
                    OnLookupDone(request);
                    break;
                }

                if (diff >= request->stateTimer)
                {
                    _abandonedLookups.push_back(std::move(request->lookupFuture));
                    FinishRequest(request, false, "The source realm database did not answer in time. Please try again later.");
                    break;
                }

                request->stateTimer -= diff;
                break;
            case CopyState::Warning:
                if (diff >= request->stateTimer)
                    StartBan(request);
                else
                    request->stateTimer -= diff;
                break;
            case CopyState::Banning:
                if (request->commitCallback && request->commitCallback->InvokeIfReady())
                    OnBanCommitted(request);
                break;
            case CopyState::WaitingLogout:
                UpdateLogoutWait(request, diff);
                break;
            case CopyState::Reading:
                if (IsFutureReady(request->readFuture))
                {
                    OnReadDone(request);
                    break;
                }

                if (diff >= request->stateTimer)
                {
                    _abandonedReads.push_back(std::move(request->readFuture));
                    FinishRequest(request, false, "Reading your character from the source realm took too long. Please try again later.");
                    break;
                }

                request->stateTimer -= diff;
                break;
            case CopyState::Committing:
                if (request->commitCallback && request->commitCallback->InvokeIfReady())
                {
                    request->commitCallback.reset();
                    if (request->commitSuccess)
                    {
                        sCharacterCache->RefreshCacheEntry(request->targetGuid.GetCounter());
                        FinishRequest(request, true, Acore::StringFormat(
                            "Copy complete! {} now holds the data of {}.",
                            request->targetName, request->source.name));
                    }
                    else
                        FinishRequest(request, false, "Writing the copied data failed. Your character was left untouched.");
                }
                break;
            default:
                break;
        }
    }

    void Mgr::StartLookup(CopyRequest& request)
    {
        NotifyRequester(request, Acore::StringFormat(
            "Cross-realm copy: your request for '{}' is now being processed.", request.sourceName));

        request.lookupFuture = std::async(std::launch::async,
            [config = _dbConfig, name = request.sourceName]()
            {
                return LookupSourceCharacter(config, name);
            });
        request.state = CopyState::LookingUp;
        request.stateTimer = LOOKUP_TIMEOUT_MS;
    }

    void Mgr::OnLookupDone(std::shared_ptr<CopyRequest> const& request)
    {
        SourceCharacter source;
        try
        {
            source = request->lookupFuture.get();
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module", "[CrossRealmCopy] Lookup of '{}' threw: {}", request->sourceName, e.what());
            FinishRequest(request, false, "The source realm database could not be reached. Please try again later.");
            return;
        }

        if (!source.error.empty())
        {
            LOG_ERROR("module", "[CrossRealmCopy] Lookup of '{}' failed: {}", request->sourceName, source.error);
            FinishRequest(request, false, "The source realm database could not be reached. Please try again later.");
            return;
        }

        if (!source.found)
        {
            FinishRequest(request, false, Acore::StringFormat(
                "No character named '{}' exists on the source realm.", request->sourceName));
            return;
        }

        if (source.online && !_dbConfig.allowOnlineSource)
        {
            FinishRequest(request, false, Acore::StringFormat(
                "{} is currently online on the source realm. Log it out there first.", source.name));
            return;
        }

        if (_requireSameAccount && !request->gmBypass && source.account != request->accountId)
        {
            FinishRequest(request, false, Acore::StringFormat(
                "{} does not belong to your account.", source.name));
            return;
        }

        if (source.race != request->race)
        {
            FinishRequest(request, false, Acore::StringFormat(
                "{} has a different race than {}. You must be logged on a character of the same race and class.",
                source.name, request->targetName));
            return;
        }

        if (source.charClass != request->charClass)
        {
            FinishRequest(request, false, Acore::StringFormat(
                "{} has a different class than {}. You must be logged on a character of the same race and class.",
                source.name, request->targetName));
            return;
        }

        request->source = source;

        if (ObjectAccessor::FindConnectedPlayer(request->targetGuid))
        {
            NotifyRequester(*request, Acore::StringFormat(
                "Copying {} from the source realm onto {}: you will be disconnected in {} seconds.",
                source.name, request->targetName, _kickDelayMs / 1000));
            request->state = CopyState::Warning;
            request->stateTimer = _kickDelayMs;
        }
        else
            // The player already logged out; no warning needed.
            StartBan(request);
    }

    void Mgr::StartBan(std::shared_ptr<CopyRequest> const& request)
    {
        // A temporary character ban keeps the target logged off while its rows are
        // rewritten, so the server can never save the character mid-copy. Committed
        // before the kick so there is no window to log back in. Removed on completion;
        // the duration only matters if the server dies mid-copy.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(Acore::StringFormat(
            "INSERT INTO character_banned (guid, bandate, unbandate, bannedby, banreason, active) "
            "VALUES ({0}, UNIX_TIMESTAMP(), UNIX_TIMESTAMP()+{1}, 'CrossRealmCopy', "
            "'Cross-realm character copy in progress', 1) "
            "ON DUPLICATE KEY UPDATE unbandate = UNIX_TIMESTAMP()+{1}, active = 1",
            request->targetGuid.GetCounter(), _banDurationSecs));

        request->commitDone = false;
        request->commitSuccess = false;
        request->commitCallback.emplace(CharacterDatabase.AsyncCommitTransaction(trans));
        request->commitCallback->AfterComplete([request](bool success)
        {
            request->commitDone = true;
            request->commitSuccess = success;
        });
        request->state = CopyState::Banning;
    }

    void Mgr::OnBanCommitted(std::shared_ptr<CopyRequest> const& request)
    {
        request->commitCallback.reset();

        if (!request->commitSuccess)
        {
            FinishRequest(request, false, "Your character could not be locked for the copy. Please try again later.");
            return;
        }

        request->banned = true;

        if (Player* player = ObjectAccessor::FindConnectedPlayer(request->targetGuid))
            player->GetSession()->KickPlayer("Cross-realm character copy");

        request->state = CopyState::WaitingLogout;
        request->stateTimer = _logoutTimeoutMs;
        request->onlinePollTimer = 0;
    }

    void Mgr::UpdateLogoutWait(std::shared_ptr<CopyRequest> const& request, uint32 diff)
    {
        if (diff >= request->stateTimer)
        {
            FinishRequest(request, false, "Your character could not be logged out in time. Please try again later.");
            return;
        }

        request->stateTimer -= diff;

        if (request->logoutSaveConfirmed)
        {
            StartRead(*request);
            return;
        }

        if (diff < request->onlinePollTimer)
        {
            request->onlinePollTimer -= diff;
            return;
        }

        request->onlinePollTimer = ONLINE_POLL_INTERVAL_MS;

        // First wait for the session to fully drop the player. A player that slipped
        // back in before the ban landed is kicked again.
        if (Player* player = ObjectAccessor::FindConnectedPlayer(request->targetGuid))
        {
            player->GetSession()->KickPlayer("Cross-realm character copy");
            return;
        }

        // ... then for the logout save to land: LogoutPlayer() enqueues the character
        // save and afterwards clears the online flag, both through the same async
        // pipeline our copy uses, so online = 0 means the save is flushed and nothing
        // will write to the character rows anymore (the ban blocks new logins).
        if (request->onlineCheckPending)
            return;

        request->onlineCheckPending = true;
        _queryProcessor.AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT online FROM characters WHERE guid = {}", request->targetGuid.GetCounter()))
            .WithCallback([request](QueryResult result)
            {
                request->onlineCheckPending = false;
                if (result && result->Fetch()[0].Get<uint8>() == 0)
                    request->logoutSaveConfirmed = true;
            }));
    }

    void Mgr::StartRead(CopyRequest& request)
    {
        request.readFuture = std::async(std::launch::async,
            [config = _dbConfig, sourceGuid = request.source.guid]()
            {
                return ReadSourceCharacter(config, sourceGuid);
            });
        request.state = CopyState::Reading;
        request.stateTimer = READ_TIMEOUT_MS;
    }

    void Mgr::OnReadDone(std::shared_ptr<CopyRequest> const& request)
    {
        CharacterSnapshot snapshot;
        try
        {
            snapshot = request->readFuture.get();
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module", "[CrossRealmCopy] Snapshot of {} threw: {}", request->source.name, e.what());
            FinishRequest(request, false, "Reading your character from the source realm failed. Your character was left untouched.");
            return;
        }

        if (!snapshot.error.empty())
        {
            LOG_ERROR("module", "[CrossRealmCopy] Snapshot of {} failed: {}", request->source.name, snapshot.error);
            FinishRequest(request, false, Acore::StringFormat(
                "The copy was aborted: {}. Your character was left untouched.", snapshot.error));
            return;
        }

        // The target may have been deleted from the character screen while the request
        // was queued; writing now would resurrect it. The cache also carries a rename
        // that happened after the request was issued.
        CharacterCacheEntry const* targetEntry = sCharacterCache->GetCharacterCacheByGuid(request->targetGuid);
        if (!targetEntry || targetEntry->AccountId != request->accountId)
        {
            FinishRequest(request, false, Acore::StringFormat(
                "{} no longer exists on this realm, the copy was aborted.", request->targetName));
            return;
        }
        request->targetName = targetEntry->Name;

        // The target's petitions die with its inventory (the charter items are wiped);
        // drop them from the in-memory store to match the DELETEs in the transaction.
        sPetitionMgr->RemovePetitionByOwnerAndType(request->targetGuid, 0);

        GuidRemaps remaps;
        remaps.sourceLow = request->source.guid;
        remaps.targetLow = request->targetGuid.GetCounter();
        remaps.targetAccount = request->accountId;
        remaps.targetName = request->targetName;
        AllocateNewIds(snapshot, remaps);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        std::string error;
        if (!BuildApplyTransaction(snapshot, remaps, trans, error))
        {
            LOG_ERROR("module", "[CrossRealmCopy] Could not build the transaction for {}: {}",
                request->source.name, error);
            FinishRequest(request, false, Acore::StringFormat(
                "The copy was aborted: {}. Your character was left untouched.", error));
            return;
        }

        LOG_INFO("module", "[CrossRealmCopy] Applying copy of {} (source guid {}) onto {} ({}): {} statements, {} items",
            request->source.name, request->source.guid, request->targetName,
            request->targetGuid.ToString(), trans->GetSize(), remaps.items.size());

        request->commitDone = false;
        request->commitSuccess = false;
        request->commitCallback.emplace(CharacterDatabase.AsyncCommitTransaction(trans));
        request->commitCallback->AfterComplete([request](bool success)
        {
            request->commitDone = true;
            request->commitSuccess = success;
        });
        request->state = CopyState::Committing;
    }

    void Mgr::FinishRequest(std::shared_ptr<CopyRequest> const& request, bool success, std::string const& message)
    {
        if (request->banned)
            // Only removes the ban this module created; bans issued by GMs stay.
            CharacterDatabase.Execute(Acore::StringFormat(
                "DELETE FROM character_banned WHERE guid = {} AND bannedby = 'CrossRealmCopy'",
                request->targetGuid.GetCounter()));

        request->finished = true;
        NotifyRequester(*request, message);

        if (success)
            LOG_INFO("module", "[CrossRealmCopy] Copied {} (source guid {}) onto {} ({}, account {})",
                request->source.name, request->source.guid, request->targetName,
                request->targetGuid.ToString(), request->accountId);
        else
            LOG_INFO("module", "[CrossRealmCopy] Request of account {} for '{}' onto {} ended without a copy: {}",
                request->accountId, request->sourceName, request->targetName, message);

        if (!_queue.empty() && _queue.front() == request)
            _queue.pop_front();

        AnnounceQueuePositions();
    }

    void Mgr::NotifyRequester(CopyRequest const& request, std::string const& message) const
    {
        // The player may be offline or on another character of the same account; the
        // session (when there is one) receives the message either way.
        if (WorldSession* session = sWorldSessionMgr->FindSession(request.accountId))
            ChatHandler(session).SendSysMessage(message);
    }

    void Mgr::AnnounceQueuePositions() const
    {
        for (std::size_t i = 1; i < _queue.size(); ++i)
            NotifyRequester(*_queue[i], Acore::StringFormat(
                "Cross-realm copy: you are now position {} in the queue.", i + 1));
    }

    bool Mgr::HandleCopyCommand(ChatHandler* handler, std::string const& sourceName)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (!_enabled)
        {
            handler->SendSysMessage("Cross-realm character copies are currently disabled.");
            return true;
        }

        if (sourceName.empty() || sourceName.size() > 12)
        {
            handler->SendSysMessage("Invalid character name.");
            return true;
        }

        uint32 accountId = player->GetSession()->GetAccountId();
        bool gmBypass = _gmBypassSecurity && uint32(player->GetSession()->GetSecurity()) >= _gmBypassSecurity;

        if (!gmBypass && _maxTargetLevel && player->GetLevel() > _maxTargetLevel)
        {
            handler->PSendSysMessage(
                "This command replaces ALL data of the character you are playing and can only be used below level {}.",
                _maxTargetLevel + 1);
            return true;
        }

        for (std::shared_ptr<CopyRequest> const& pending : _queue)
        {
            if (pending->targetGuid == player->GetGUID())
            {
                handler->SendSysMessage("This character already has a pending copy request.");
                return true;
            }

            if (pending->accountId == accountId)
            {
                handler->SendSysMessage("Your account already has a pending copy request.");
                return true;
            }
        }

        if (_queue.size() >= _maxQueueSize)
        {
            handler->SendSysMessage("The copy queue is full right now, please try again in a few minutes.");
            return true;
        }

        auto request = std::make_shared<CopyRequest>();
        request->id = _nextRequestId++;
        request->targetGuid = player->GetGUID();
        request->accountId = accountId;
        request->targetName = player->GetName();
        request->sourceName = sourceName;
        request->race = player->getRace();
        request->charClass = player->getClass();
        request->gmBypass = gmBypass;
        _queue.push_back(request);

        LOG_INFO("module", "[CrossRealmCopy] Account {} queued a copy of '{}' onto {} ({}), queue size {}",
            accountId, sourceName, request->targetName, request->targetGuid.ToString(), _queue.size());

        if (_queue.size() == 1)
            handler->SendSysMessage("Copy request accepted, it will start in a moment.");
        else
            handler->PSendSysMessage("Copy request accepted: you are position {} in the queue.", _queue.size());

        return true;
    }
}
