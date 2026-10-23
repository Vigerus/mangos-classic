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
class Player;
class Unit;

// the subscribed player joined a group (team)
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnJoinedTeam, Group* /*group*/, ObjectGuid /*self*/);
// the subscribed player left its group, or the group was disbanded; the group is still valid during the call
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnLeftTeam, Group* /*group*/, ObjectGuid /*self*/);
// another player joined the subscribed player's group
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMemberJoinedTeam, Group* /*group*/, ObjectGuid /*self*/, ObjectGuid /*member*/);
// another player left the subscribed player's group
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMemberLeftTeam, Group* /*group*/, ObjectGuid /*self*/, ObjectGuid /*member*/);
// the subscribed player entered combat (or something announced it will: a bot pulling or attacking)
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCombatStarted, ObjectGuid /*self*/);
// the subscribed player left combat (also when it dies: before OnDied)
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCombatEnded, ObjectGuid /*self*/);
// another member of the subscribed player's group, on the same map, entered combat
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMemberCombatStarted, ObjectGuid /*self*/, Player* /*member*/);
// another member of the subscribed player's group, on the same map, left combat
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMemberCombatEnded, ObjectGuid /*self*/, Player* /*member*/);
// a unit put the subscribed player (or its pet) on its threat list or started melee attacking it / took it off,
// stopped (each link is reported on its own: a creature attacking a player usually adds both)
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAttackerAdded, ObjectGuid /*self*/, Unit* /*attacker*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAttackerRemoved, ObjectGuid /*self*/, Unit* /*attacker*/);
// the same for another member of the subscribed player's group (or its pet) on the same map
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMemberAttackerAdded, ObjectGuid /*self*/, Player* /*member*/, Unit* /*attacker*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMemberAttackerRemoved, ObjectGuid /*self*/, Player* /*member*/, Unit* /*attacker*/);
// the subscribed player was alive and is not any more
DECLARE_MULTICAST_DELEGATE_OneParam(FOnDied, ObjectGuid /*self*/);
// the subscribed player was dead (corpse, ghost) and is alive again
DECLARE_MULTICAST_DELEGATE_OneParam(FOnResurrected, ObjectGuid /*self*/);
// the subscribed player learned or lost talents (its spec may have changed)
DECLARE_MULTICAST_DELEGATE_OneParam(FOnTalentsChanged, ObjectGuid /*self*/);
// the same for another member of the subscribed player's group on the same map
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMemberTalentsChanged, ObjectGuid /*self*/, Player* /*member*/);
// the group started a loot roll the subscribed player may vote on (need / greed / pass): the looted object, the
// item's slot in its loot, the item and the votes allowed to the player (RollVoteMask)
DECLARE_MULTICAST_DELEGATE_FiveParams(FOnLootRollStarted, ObjectGuid /*self*/, ObjectGuid /*lootGuid*/, uint32 /*itemSlot*/, uint32 /*itemId*/, uint8 /*voteMask*/);

/*
 * Group membership, combat and life events of single players, subscribed by the player's guid ("self"):
 *
 *   TeamSubsystem* teams = sWorld.GetSubsystem<TeamSubsystem>();
 *   teams->AddOnMemberJoinedTeam(bot->GetObjectGuid(), this, &Foo::OnMemberJoined);
 *   teams->AddOnLeftTeam(bot->GetObjectGuid(), [](Group* group, ObjectGuid self) { ... });
 *   teams->RemoveAll(bot->GetObjectGuid(), this);
 *
 * Callbacks run on the thread that changes the group (the world thread for player commands, the bot's map
 * thread for bot invites), or the player's map thread for combat and life events (member combat events only
 * reach members on that map), while the subsystem's lock is held: keep them short. Subscribing and
 * unsubscribing from inside a callback is allowed.
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
        template <typename... Args>
        FDelegateHandle AddOnCombatStarted(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onCombatStarted, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnCombatEnded(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onCombatEnded, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnMemberCombatStarted(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onMemberCombatStarted, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnMemberCombatEnded(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onMemberCombatEnded, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnAttackerAdded(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onAttackerAdded, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnAttackerRemoved(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onAttackerRemoved, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnMemberAttackerAdded(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onMemberAttackerAdded, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnMemberAttackerRemoved(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onMemberAttackerRemoved, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnDied(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onDied, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnResurrected(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onResurrected, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnTalentsChanged(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onTalentsChanged, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnMemberTalentsChanged(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onMemberTalentsChanged, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnLootRollStarted(ObjectGuid self, Args&&... args) { return AddTo(self, &PlayerDelegates::onLootRollStarted, std::forward<Args>(args)...); }

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
        // called by Unit where the combat flag of a player changes, and by bots that start a fight: OnCombatStarted /
        // OnCombatEnded for player, OnMemberCombatStarted / OnMemberCombatEnded for the group members on its map
        void BroadcastCombatStarted(Player* player);
        void BroadcastCombatEnded(Player* player);
        // called by Unit where a unit puts victim (a player or its pet) on its threat list / takes it off: OnAttackerAdded /
        // OnAttackerRemoved for victim, OnMemberAttackerAdded / OnMemberAttackerRemoved for the group members on its map
        void BroadcastAttackerAdded(Player* victim, Unit* attacker);
        void BroadcastAttackerRemoved(Player* victim, Unit* attacker);
        // called by Unit where the death state of a player changes: OnDied / OnResurrected for player
        void BroadcastDied(Player* player);
        void BroadcastResurrected(Player* player);
        // called by Player where a talent spell is learned or removed (in world only): OnTalentsChanged for player,
        // OnMemberTalentsChanged for the group members on its map
        void BroadcastTalentsChanged(Player* player);
        // called by GroupLootRoll when it sends the start of a roll to a voter: OnLootRollStarted for player
        void BroadcastLootRollStarted(Player* player, ObjectGuid lootGuid, uint32 itemSlot, uint32 itemId, uint8 voteMask);

        virtual void Deinitialize() override;

    private:
        struct PlayerDelegates
        {
            FOnJoinedTeam onJoinedTeam;
            FOnLeftTeam onLeftTeam;
            FOnMemberJoinedTeam onMemberJoinedTeam;
            FOnMemberLeftTeam onMemberLeftTeam;
            FOnCombatStarted onCombatStarted;
            FOnCombatEnded onCombatEnded;
            FOnMemberCombatStarted onMemberCombatStarted;
            FOnMemberCombatEnded onMemberCombatEnded;
            FOnAttackerAdded onAttackerAdded;
            FOnAttackerRemoved onAttackerRemoved;
            FOnMemberAttackerAdded onMemberAttackerAdded;
            FOnMemberAttackerRemoved onMemberAttackerRemoved;
            FOnDied onDied;
            FOnResurrected onResurrected;
            FOnTalentsChanged onTalentsChanged;
            FOnMemberTalentsChanged onMemberTalentsChanged;
            FOnLootRollStarted onLootRollStarted;

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
        // the other members of player's group that are on its map (they run on the same map thread)
        static std::vector<Player*> GetMembersOnMap(Player* player);
        // the self delegate (self, params...) for player, then the member delegate (other, player, params...) for the
        // members on its map
        template <typename SelfDelegateType, typename MemberDelegateType, typename... Params>
        void BroadcastToTeam(Player* player, SelfDelegateType PlayerDelegates::* self, MemberDelegateType PlayerDelegates::* member, Params... params);
        // drops self's entry when nothing is bound to it any more (not while a broadcast runs)
        void CleanupPlayer(PlayerMap::iterator itr);

        std::recursive_mutex m_lock;
        PlayerMap m_players;
        uint32 m_broadcastDepth = 0;
        // lets broadcasts skip the lock while nobody subscribed
        std::atomic<bool> m_hasPlayers{false};
};

#endif
