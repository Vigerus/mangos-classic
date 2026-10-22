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
#include "Entities/Player.h"

bool TeamSubsystem::PlayerDelegates::IsBound() const
{
    return onJoinedTeam.IsBound() || onLeftTeam.IsBound() || onMemberJoinedTeam.IsBound() || onMemberLeftTeam.IsBound() ||
           onCombatStarted.IsBound() || onCombatEnded.IsBound() || onMemberCombatStarted.IsBound() || onMemberCombatEnded.IsBound() ||
           onAttackerAdded.IsBound() || onAttackerRemoved.IsBound() || onMemberAttackerAdded.IsBound() || onMemberAttackerRemoved.IsBound() ||
           onDied.IsBound() || onResurrected.IsBound() || onLootRollStarted.IsBound();
}

bool TeamSubsystem::Remove(ObjectGuid self, FDelegateHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(self);
    if (itr == m_players.end())
        return false;

    PlayerDelegates& delegates = itr->second;
    bool const removed = delegates.onJoinedTeam.Remove(handle) || delegates.onLeftTeam.Remove(handle) ||
                         delegates.onMemberJoinedTeam.Remove(handle) || delegates.onMemberLeftTeam.Remove(handle) ||
                         delegates.onCombatStarted.Remove(handle) || delegates.onCombatEnded.Remove(handle) ||
                         delegates.onMemberCombatStarted.Remove(handle) || delegates.onMemberCombatEnded.Remove(handle) ||
                         delegates.onAttackerAdded.Remove(handle) || delegates.onAttackerRemoved.Remove(handle) ||
                         delegates.onMemberAttackerAdded.Remove(handle) || delegates.onMemberAttackerRemoved.Remove(handle) ||
                         delegates.onDied.Remove(handle) || delegates.onResurrected.Remove(handle);
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
                           delegates.onMemberJoinedTeam.RemoveAll(obj) + delegates.onMemberLeftTeam.RemoveAll(obj) +
                           delegates.onCombatStarted.RemoveAll(obj) + delegates.onCombatEnded.RemoveAll(obj) +
                           delegates.onMemberCombatStarted.RemoveAll(obj) + delegates.onMemberCombatEnded.RemoveAll(obj) +
                           delegates.onAttackerAdded.RemoveAll(obj) + delegates.onAttackerRemoved.RemoveAll(obj) +
                           delegates.onMemberAttackerAdded.RemoveAll(obj) + delegates.onMemberAttackerRemoved.RemoveAll(obj) +
                           delegates.onDied.RemoveAll(obj) + delegates.onResurrected.RemoveAll(obj);
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

std::vector<Player*> TeamSubsystem::GetMembersOnMap(Player* player)
{
    std::vector<Player*> members;
    if (Group* group = player->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->getSource())
                if (member != player && member->IsInMap(player))
                    members.push_back(member);
    return members;
}

template <typename SelfDelegateType, typename MemberDelegateType, typename... Params>
void TeamSubsystem::BroadcastToTeam(Player* player, SelfDelegateType PlayerDelegates::* self, MemberDelegateType PlayerDelegates::* member, Params... params)
{
    if (!m_hasPlayers || !player)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    ObjectGuid const guid = player->GetObjectGuid();
    Broadcast(guid, self, guid, params...);
    for (Player* other : GetMembersOnMap(player))
        Broadcast(other->GetObjectGuid(), member, other->GetObjectGuid(), player, params...);
}

void TeamSubsystem::BroadcastCombatStarted(Player* player)
{
    BroadcastToTeam(player, &PlayerDelegates::onCombatStarted, &PlayerDelegates::onMemberCombatStarted);
}

void TeamSubsystem::BroadcastCombatEnded(Player* player)
{
    BroadcastToTeam(player, &PlayerDelegates::onCombatEnded, &PlayerDelegates::onMemberCombatEnded);
}

void TeamSubsystem::BroadcastAttackerAdded(Player* victim, Unit* attacker)
{
    if (attacker)
        BroadcastToTeam(victim, &PlayerDelegates::onAttackerAdded, &PlayerDelegates::onMemberAttackerAdded, attacker);
}

void TeamSubsystem::BroadcastAttackerRemoved(Player* victim, Unit* attacker)
{
    if (attacker)
        BroadcastToTeam(victim, &PlayerDelegates::onAttackerRemoved, &PlayerDelegates::onMemberAttackerRemoved, attacker);
}

void TeamSubsystem::BroadcastDied(Player* player)
{
    if (!m_hasPlayers || !player)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    Broadcast(player->GetObjectGuid(), &PlayerDelegates::onDied, player->GetObjectGuid());
}

void TeamSubsystem::BroadcastLootRollStarted(Player* player, ObjectGuid lootGuid, uint32 itemSlot, uint32 itemId, uint8 voteMask)
{
    if (!m_hasPlayers || !player)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    Broadcast(player->GetObjectGuid(), &PlayerDelegates::onLootRollStarted, player->GetObjectGuid(), lootGuid, itemSlot, itemId, voteMask);
}

void TeamSubsystem::BroadcastResurrected(Player* player)
{
    if (!m_hasPlayers || !player)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    Broadcast(player->GetObjectGuid(), &PlayerDelegates::onResurrected, player->GetObjectGuid());
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
