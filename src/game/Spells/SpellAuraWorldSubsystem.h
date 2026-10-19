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

#ifndef MANGOS_SPELLAURAWORLDSUBSYSTEM_H
#define MANGOS_SPELLAURAWORLDSUBSYSTEM_H

#include "World/WorldSubsystem.h"
#include "delegates/Delegates.h"
#include "Entities/ObjectGuid.h"
#include "Entities/Unit.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

class SpellAuraHolder;

// an aura (spell aura holder) appeared on target
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAuraApplied, Unit* /*target*/, SpellAuraHolder* /*holder*/);
// an aura disappeared from target; the holder is already out of the target's aura lists (deleted later)
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAuraRemoved, Unit* /*target*/, SpellAuraHolder* /*holder*/, AuraRemoveMode /*mode*/);
// the stack amount of an aura on target changed
DECLARE_MULTICAST_DELEGATE_FourParams(FOnAuraStackChanged, Unit* /*target*/, SpellAuraHolder* /*holder*/, uint32 /*oldStackAmount*/, uint32 /*newStackAmount*/);
// the proc charges of an aura on target changed
DECLARE_MULTICAST_DELEGATE_FourParams(FOnAuraChargesChanged, Unit* /*target*/, SpellAuraHolder* /*holder*/, uint32 /*oldCharges*/, uint32 /*newCharges*/);

/*
 * Aura events of single units, subscribed by the unit's guid:
 *
 *   SpellAuraWorldSubsystem* auras = sWorld.GetSubsystem<SpellAuraWorldSubsystem>();
 *   FDelegateHandle handle = auras->AddOnAuraApplied(unit->GetObjectGuid(), this, &Foo::OnAuraApplied);
 *   auras->AddOnAuraRemoved(unit->GetObjectGuid(), [](Unit* target, SpellAuraHolder* holder, AuraRemoveMode mode) { ... });
 *   auras->Remove(unit->GetObjectGuid(), handle);
 *   auras->RemoveAll(unit->GetObjectGuid(), this);      // every member binding of this object
 *
 * Callbacks run on the thread that changes the target's auras (the target's map thread) while the
 * subsystem's lock is held: keep them short and don't wait on other locks in them. Subscribing and
 * unsubscribing from inside a callback is allowed.
 */
class SpellAuraWorldSubsystem : public WorldSubsystem
{
    public:
        template <typename... Args>
        FDelegateHandle AddOnAuraApplied(ObjectGuid target, Args&&... args) { return AddTo(target, &TargetDelegates::onAuraApplied, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnAuraRemoved(ObjectGuid target, Args&&... args) { return AddTo(target, &TargetDelegates::onAuraRemoved, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnAuraStackChanged(ObjectGuid target, Args&&... args) { return AddTo(target, &TargetDelegates::onAuraStackChanged, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnAuraChargesChanged(ObjectGuid target, Args&&... args) { return AddTo(target, &TargetDelegates::onAuraChargesChanged, std::forward<Args>(args)...); }

        // unbinds the binding identified by handle (made by any of the AddOn* of target)
        bool Remove(ObjectGuid target, FDelegateHandle handle);
        // unbinds every member function binding of obj on target, returns the number removed
        uint32 RemoveAll(ObjectGuid target, const void* obj);

        // called by the core where auras change
        void BroadcastAuraApplied(Unit* target, SpellAuraHolder* holder);
        void BroadcastAuraRemoved(Unit* target, SpellAuraHolder* holder, AuraRemoveMode mode);
        void BroadcastAuraStackChanged(Unit* target, SpellAuraHolder* holder, uint32 oldStackAmount, uint32 newStackAmount);
        void BroadcastAuraChargesChanged(Unit* target, SpellAuraHolder* holder, uint32 oldCharges, uint32 newCharges);

        virtual void Deinitialize() override;

    private:
        struct TargetDelegates
        {
            FOnAuraApplied onAuraApplied;
            FOnAuraRemoved onAuraRemoved;
            FOnAuraStackChanged onAuraStackChanged;
            FOnAuraChargesChanged onAuraChargesChanged;

            bool IsBound() const;
        };

        using TargetMap = std::unordered_map<ObjectGuid, TargetDelegates>;

        // one argument binds a callable, two an object and its member function
        template <typename DelegateType, typename... Args>
        FDelegateHandle AddTo(ObjectGuid target, DelegateType TargetDelegates::* member, Args&&... args)
        {
            static_assert(sizeof...(Args) == 1 || sizeof...(Args) == 2, "pass a callable, or an object and its member function");
            std::lock_guard<std::recursive_mutex> lock(m_lock);
            DelegateType& delegate = m_targets[target].*member;
            m_hasTargets = true;
            if constexpr (sizeof...(Args) == 1)
                return delegate.AddLambda(std::forward<Args>(args)...);
            else
                return delegate.Add(std::forward<Args>(args)...);
        }

        template <typename DelegateType, typename... Params>
        void Broadcast(Unit* target, DelegateType TargetDelegates::* member, Params... params);

        // drops target's entry when nothing is bound to it any more (not while a broadcast runs)
        void CleanupTarget(TargetMap::iterator itr);

        std::recursive_mutex m_lock;
        TargetMap m_targets;
        uint32 m_broadcastDepth = 0;
        // lets broadcasts skip the lock while nobody subscribed
        std::atomic<bool> m_hasTargets{false};
};

#endif
