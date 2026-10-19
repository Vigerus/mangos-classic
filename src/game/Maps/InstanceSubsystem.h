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

#ifndef MANGOS_INSTANCESUBSYSTEM_H
#define MANGOS_INSTANCESUBSYSTEM_H

#include "World/WorldSubsystem.h"
#include "delegates/Delegates.h"
#include "Entities/ObjectGuid.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

class Map;
class Player;

// the subscribed player was added to a map (world map, dungeon or battleground instance), it is in the world
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPlayerEnterMap, Player* /*player*/, Map* /*map*/);
// the subscribed player is being removed from a map (teleport, logout), still in the world during the call
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPlayerLeaveMap, Player* /*player*/, Map* /*map*/);

/*
 * Map / instance changes of single players, subscribed by the player's guid:
 *
 *   InstanceSubsystem* instances = sWorld.GetSubsystem<InstanceSubsystem>();
 *   instances->AddOnPlayerEnterMap(bot->GetObjectGuid(), this, &Foo::OnEnterMap);
 *   instances->AddOnPlayerLeaveMap(bot->GetObjectGuid(), [](Player* player, Map* map) { ... });
 *   instances->RemoveAll(bot->GetObjectGuid(), this);
 *
 * Broadcast from Map::Add(Player*) / Map::Remove(Player*) (every map type), next to InstanceData::OnPlayerEnter /
 * OnPlayerLeave. Callbacks run on the thread that moves the player while the subsystem's lock is held: keep them
 * short. Subscribing and unsubscribing from inside a callback is allowed.
 */
class InstanceSubsystem : public WorldSubsystem
{
    public:
        template <typename... Args>
        FDelegateHandle AddOnPlayerEnterMap(ObjectGuid player, Args&&... args) { return AddTo(player, &PlayerDelegates::onPlayerEnterMap, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnPlayerLeaveMap(ObjectGuid player, Args&&... args) { return AddTo(player, &PlayerDelegates::onPlayerLeaveMap, std::forward<Args>(args)...); }

        // unbinds the binding identified by handle (made by any of the AddOn* of player)
        bool Remove(ObjectGuid player, FDelegateHandle handle);
        // unbinds every member function binding of obj on player, returns the number removed
        uint32 RemoveAll(ObjectGuid player, const void* obj);

        // called by Map
        void BroadcastPlayerEnterMap(Player* player, Map* map);
        void BroadcastPlayerLeaveMap(Player* player, Map* map);

        virtual void Deinitialize() override;

    private:
        struct PlayerDelegates
        {
            FOnPlayerEnterMap onPlayerEnterMap;
            FOnPlayerLeaveMap onPlayerLeaveMap;

            bool IsBound() const { return onPlayerEnterMap.IsBound() || onPlayerLeaveMap.IsBound(); }
        };

        using PlayerMap = std::unordered_map<ObjectGuid, PlayerDelegates>;

        // one argument binds a callable, two an object and its member function
        template <typename DelegateType, typename... Args>
        FDelegateHandle AddTo(ObjectGuid player, DelegateType PlayerDelegates::* member, Args&&... args)
        {
            static_assert(sizeof...(Args) == 1 || sizeof...(Args) == 2, "pass a callable, or an object and its member function");
            std::lock_guard<std::recursive_mutex> lock(m_lock);
            DelegateType& delegate = m_players[player].*member;
            m_hasPlayers = true;
            if constexpr (sizeof...(Args) == 1)
                return delegate.AddLambda(std::forward<Args>(args)...);
            else
                return delegate.Add(std::forward<Args>(args)...);
        }

        template <typename DelegateType>
        void Broadcast(Player* player, Map* map, DelegateType PlayerDelegates::* member);

        // drops the player's entry when nothing is bound to it any more (not while a broadcast runs)
        void CleanupPlayer(PlayerMap::iterator itr);

        std::recursive_mutex m_lock;
        PlayerMap m_players;
        uint32 m_broadcastDepth = 0;
        // lets broadcasts skip the lock while nobody subscribed
        std::atomic<bool> m_hasPlayers{false};
};

#endif
