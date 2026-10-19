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

#include "Spells/SpellAuraWorldSubsystem.h"

bool SpellAuraWorldSubsystem::TargetDelegates::IsBound() const
{
    return onAuraApplied.IsBound() || onAuraRemoved.IsBound() || onAuraStackChanged.IsBound() || onAuraChargesChanged.IsBound();
}

bool SpellAuraWorldSubsystem::Remove(ObjectGuid target, FDelegateHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_targets.find(target);
    if (itr == m_targets.end())
        return false;

    TargetDelegates& delegates = itr->second;
    bool const removed = delegates.onAuraApplied.Remove(handle) || delegates.onAuraRemoved.Remove(handle) ||
                         delegates.onAuraStackChanged.Remove(handle) || delegates.onAuraChargesChanged.Remove(handle);
    CleanupTarget(itr);
    return removed;
}

uint32 SpellAuraWorldSubsystem::RemoveAll(ObjectGuid target, const void* obj)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_targets.find(target);
    if (itr == m_targets.end())
        return 0;

    TargetDelegates& delegates = itr->second;
    uint32 const removed = delegates.onAuraApplied.RemoveAll(obj) + delegates.onAuraRemoved.RemoveAll(obj) +
                           delegates.onAuraStackChanged.RemoveAll(obj) + delegates.onAuraChargesChanged.RemoveAll(obj);
    CleanupTarget(itr);
    return removed;
}

template <typename DelegateType, typename... Params>
void SpellAuraWorldSubsystem::Broadcast(Unit* target, DelegateType TargetDelegates::* member, Params... params)
{
    if (!m_hasTargets || !target)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_targets.find(target->GetObjectGuid());
    if (itr == m_targets.end())
        return;

    // entries are not erased while a broadcast runs, so itr stays valid even if callbacks (un)subscribe
    ++m_broadcastDepth;
    (itr->second.*member).Broadcast(target, params...);
    --m_broadcastDepth;
    CleanupTarget(itr);
}

void SpellAuraWorldSubsystem::BroadcastAuraApplied(Unit* target, SpellAuraHolder* holder)
{
    Broadcast(target, &TargetDelegates::onAuraApplied, holder);
}

void SpellAuraWorldSubsystem::BroadcastAuraRemoved(Unit* target, SpellAuraHolder* holder, AuraRemoveMode mode)
{
    Broadcast(target, &TargetDelegates::onAuraRemoved, holder, mode);
}

void SpellAuraWorldSubsystem::BroadcastAuraStackChanged(Unit* target, SpellAuraHolder* holder, uint32 oldStackAmount, uint32 newStackAmount)
{
    Broadcast(target, &TargetDelegates::onAuraStackChanged, holder, oldStackAmount, newStackAmount);
}

void SpellAuraWorldSubsystem::BroadcastAuraChargesChanged(Unit* target, SpellAuraHolder* holder, uint32 oldCharges, uint32 newCharges)
{
    Broadcast(target, &TargetDelegates::onAuraChargesChanged, holder, oldCharges, newCharges);
}

void SpellAuraWorldSubsystem::Deinitialize()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_targets.clear();
    m_hasTargets = false;
}

void SpellAuraWorldSubsystem::CleanupTarget(TargetMap::iterator itr)
{
    if (m_broadcastDepth || itr->second.IsBound())
        return;

    m_targets.erase(itr);
    m_hasTargets = !m_targets.empty();
}
