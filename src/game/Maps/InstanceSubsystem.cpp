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

#include "Maps/InstanceSubsystem.h"
#include "Entities/Player.h"

bool InstanceSubsystem::Remove(ObjectGuid player, FDelegateHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(player);
    if (itr == m_players.end())
        return false;

    bool const removed = itr->second.onPlayerEnterMap.Remove(handle) || itr->second.onPlayerLeaveMap.Remove(handle);
    CleanupPlayer(itr);
    return removed;
}

uint32 InstanceSubsystem::RemoveAll(ObjectGuid player, const void* obj)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(player);
    if (itr == m_players.end())
        return 0;

    uint32 const removed = itr->second.onPlayerEnterMap.RemoveAll(obj) + itr->second.onPlayerLeaveMap.RemoveAll(obj);
    CleanupPlayer(itr);
    return removed;
}

template <typename DelegateType>
void InstanceSubsystem::Broadcast(Player* player, Map* map, DelegateType PlayerDelegates::* member)
{
    if (!m_hasPlayers || !player)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(player->GetObjectGuid());
    if (itr == m_players.end())
        return;

    // entries are not erased while a broadcast runs, so itr stays valid even if callbacks (un)subscribe
    ++m_broadcastDepth;
    (itr->second.*member).Broadcast(player, map);
    --m_broadcastDepth;
    CleanupPlayer(itr);
}

void InstanceSubsystem::BroadcastPlayerEnterMap(Player* player, Map* map)
{
    Broadcast(player, map, &PlayerDelegates::onPlayerEnterMap);
}

void InstanceSubsystem::BroadcastPlayerLeaveMap(Player* player, Map* map)
{
    Broadcast(player, map, &PlayerDelegates::onPlayerLeaveMap);
}

void InstanceSubsystem::Deinitialize()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_players.clear();
    m_hasPlayers = false;
}

void InstanceSubsystem::CleanupPlayer(PlayerMap::iterator itr)
{
    if (m_broadcastDepth || itr->second.IsBound())
        return;

    m_players.erase(itr);
    m_hasPlayers = !m_players.empty();
}
