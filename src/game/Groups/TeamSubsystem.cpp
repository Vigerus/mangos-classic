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

#include "Groups/TeamSubsystem.h"
#include "Groups/Group.h"

bool TeamSubsystem::PlayerDelegates::IsBound() const
{
    return onJoinedTeam.IsBound() || onLeftTeam.IsBound() || onMemberJoinedTeam.IsBound() || onMemberLeftTeam.IsBound();
}

bool TeamSubsystem::Remove(ObjectGuid self, FDelegateHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(self);
    if (itr == m_players.end())
        return false;

    PlayerDelegates& delegates = itr->second;
    bool const removed = delegates.onJoinedTeam.Remove(handle) || delegates.onLeftTeam.Remove(handle) ||
                         delegates.onMemberJoinedTeam.Remove(handle) || delegates.onMemberLeftTeam.Remove(handle);
    CleanupPlayer(itr);
    return removed;
}

uint32 TeamSubsystem::RemoveAll(ObjectGuid self, const void* obj)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(self);
    if (itr == m_players.end())
        return 0;

    PlayerDelegates& delegates = itr->second;
    uint32 const removed = delegates.onJoinedTeam.RemoveAll(obj) + delegates.onLeftTeam.RemoveAll(obj) +
                           delegates.onMemberJoinedTeam.RemoveAll(obj) + delegates.onMemberLeftTeam.RemoveAll(obj);
    CleanupPlayer(itr);
    return removed;
}

template <typename DelegateType, typename... Params>
void TeamSubsystem::Broadcast(ObjectGuid self, DelegateType PlayerDelegates::* member, Params... params)
{
    auto itr = m_players.find(self);
    if (itr == m_players.end())
        return;

    // entries are not erased while a broadcast runs, so itr stays valid even if callbacks (un)subscribe
    ++m_broadcastDepth;
    (itr->second.*member).Broadcast(params...);
    --m_broadcastDepth;
    CleanupPlayer(itr);
}

std::vector<ObjectGuid> TeamSubsystem::GetMembers(Group* group)
{
    std::vector<ObjectGuid> members;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
        members.push_back(slot.guid);
    return members;
}

void TeamSubsystem::BroadcastMemberJoined(Group* group, ObjectGuid member)
{
    if (!m_hasPlayers || !group)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    Broadcast(member, &PlayerDelegates::onJoinedTeam, group, member);
    for (ObjectGuid other : GetMembers(group))
        if (other != member)
            Broadcast(other, &PlayerDelegates::onMemberJoinedTeam, group, other, member);
}

void TeamSubsystem::BroadcastMemberLeft(Group* group, ObjectGuid member)
{
    if (!m_hasPlayers || !group)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    Broadcast(member, &PlayerDelegates::onLeftTeam, group, member);
    for (ObjectGuid other : GetMembers(group))
        if (other != member)
            Broadcast(other, &PlayerDelegates::onMemberLeftTeam, group, other, member);
}

void TeamSubsystem::BroadcastDisband(Group* group)
{
    if (!m_hasPlayers || !group)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    for (ObjectGuid member : GetMembers(group))
        Broadcast(member, &PlayerDelegates::onLeftTeam, group, member);
}

void TeamSubsystem::Deinitialize()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_players.clear();
    m_hasPlayers = false;
}

void TeamSubsystem::CleanupPlayer(PlayerMap::iterator itr)
{
    if (m_broadcastDepth || itr->second.IsBound())
        return;

    m_players.erase(itr);
    m_hasPlayers = !m_players.empty();
}
