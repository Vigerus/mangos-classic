/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MANGOS_TEAMSUBSYSTEM_H
#define MANGOS_TEAMSUBSYSTEM_H

#include "World/WorldSubsystem.h"
#include "delegates/Delegates.h"
#include "Entities/ObjectGuid.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

class Group;

// the subscribed player joined a group (team)
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnJoinedTeam, Group* /*group*/, ObjectGuid /*self*/);
// the subscribed player left its group, or the group was disbanded; the group is still valid during the call
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnLeftTeam, Group* /*group*/, ObjectGuid /*self*/);
// another player joined the subscribed player's group
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMemberJoinedTeam, Group* /*group*/, ObjectGuid /*self*/, ObjectGuid /*member*/);
// another player left the subscribed player's group
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMemberLeftTeam, Group* /*group*/, ObjectGuid /*self*/, ObjectGuid /*member*/);

/*
 * Group membership events of single players, subscribed by the player's guid ("self"):
 *
 *   TeamSubsystem* teams = sWorld.GetSubsystem<TeamSubsystem>();
 *   teams->AddOnMemberJoinedTeam(bot->GetObjectGuid(), this, &Foo::OnMemberJoined);
 *   teams->AddOnLeftTeam(bot->GetObjectGuid(), [](Group* group, ObjectGuid self) { ... });
 *   teams->RemoveAll(bot->GetObjectGuid(), this);
 *
 * Callbacks run on the thread that changes the group (the world thread for player commands, the bot's map
 * thread for bot invites) while the subsystem's lock is held: keep them short. Subscribing and unsubscribing
 * from inside a callback is allowed.
 */
class TeamSubsystem : public WorldSubsystem
{
    public:
        template <typename... Args>
        FDelegateHandle AddOnJoinedTeam(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onJoinedTeam, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnLeftTeam(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onLeftTeam, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnMemberJoinedTeam(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onMemberJoinedTeam, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnMemberLeftTeam(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onMemberLeftTeam, std::forward<Args>(args)...); }

        // unbinds the binding identified by handle (made by any of the AddOn* of self)
        bool Remove(ObjectGuid self, FDelegateHandle handle);
        // unbinds every member function binding of obj on self, returns the number removed
        uint32 RemoveAll(ObjectGuid self, const void* obj);

        // called by Group: member joined group (OnJoinedTeam for it, OnMemberJoinedTeam for the other members)
        void BroadcastMemberJoined(Group* group, ObjectGuid member);
        // called by Group: member left group, already removed from it (OnLeftTeam for it, OnMemberLeftTeam for the rest)
        void BroadcastMemberLeft(Group* group, ObjectGuid member);
        // called by Group before it is disbanded: OnLeftTeam for every member
        void BroadcastDisband(Group* group);

        virtual void Deinitialize() override;

    private:
        struct PlayerDelegates
        {
            FOnJoinedTeam onJoinedTeam;
            FOnLeftTeam onLeftTeam;
            FOnMemberJoinedTeam onMemberJoinedTeam;
            FOnMemberLeftTeam onMemberLeftTeam;

            bool IsBound() const;
        };

        using PlayerMap = std::unordered_map<ObjectGuid, PlayerDelegates>;

        // one argument binds a callable, two an object and its member function
        template <typename DelegateType, typename... Args>
        FDelegateHandle AddTo(ObjectGuid self, DelegateType PlayerDelegates::* member, Args&&... args)
        {
            static_assert(sizeof...(Args) == 1 || sizeof...(Args) == 2, "pass a callable, or an object and its member function");
            std::lock_guard<std::recursive_mutex> lock(m_lock);
            DelegateType& delegate = m_players[self].*member;
            m_hasPlayers = true;
            if constexpr (sizeof...(Args) == 1)
                return delegate.AddLambda(std::forward<Args>(args)...);
            else
                return delegate.Add(std::forward<Args>(args)...);
        }

        template <typename DelegateType, typename... Params>
        void Broadcast(ObjectGuid self, DelegateType PlayerDelegates::* member, Params... params);

        static std::vector<ObjectGuid> GetMembers(Group* group);
        // drops self's entry when nothing is bound to it any more (not while a broadcast runs)
        void CleanupPlayer(PlayerMap::iterator itr);

        std::recursive_mutex m_lock;
        PlayerMap m_players;
        uint32 m_broadcastDepth = 0;
        // lets broadcasts skip the lock while nobody subscribed
        std::atomic<bool> m_hasPlayers{false};
};

#endif
