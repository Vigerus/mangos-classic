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

#ifndef MANGOS_DELEGATES_H
#define MANGOS_DELEGATES_H

#include "Platform/Define.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

/*
 * Unreal-style multicast delegates.
 *
 *   DECLARE_MULTICAST_DELEGATE_OneParam(FOnHealthChanged, uint32);
 *
 *   FOnHealthChanged OnHealthChanged;                                  // owner
 *   FDelegateHandle h1 = OnHealthChanged.Add(this, &Foo::HandleHealth);  // member (const or not)
 *   FDelegateHandle h2 = OnHealthChanged.AddLambda([](uint32 hp) { ... });
 *   OnHealthChanged.Broadcast(100);
 *   OnHealthChanged.Remove(h1);
 *   OnHealthChanged.RemoveAll(this);                                   // every member binding of this object
 *
 * Subscribers may Add/Remove (including removing themselves) while a Broadcast is running:
 * removed subscribers that have not been called yet are skipped, subscribers added during
 * the broadcast are first called on the next Broadcast. Destroying the delegate itself
 * from inside its own Broadcast is not supported. Delegates are not thread-safe.
 */

// Identifies one binding of a multicast delegate. Ids are unique for the process lifetime.
class FDelegateHandle
{
public:
    FDelegateHandle() : m_id(0) {}

    static FDelegateHandle GenerateNewHandle();

    bool IsValid() const { return m_id != 0; }
    void Reset() { m_id = 0; }
    uint64 GetId() const { return m_id; }

    bool operator==(const FDelegateHandle& other) const { return m_id == other.m_id; }
    bool operator!=(const FDelegateHandle& other) const { return m_id != other.m_id; }
    bool operator<(const FDelegateHandle& other) const { return m_id < other.m_id; }

private:
    explicit FDelegateHandle(uint64 id) : m_id(id) {}

    uint64 m_id;
};

namespace std
{
    template <>
    struct hash<FDelegateHandle>
    {
        size_t operator()(const FDelegateHandle& handle) const noexcept { return std::hash<uint64>()(handle.GetId()); }
    };
}

template <typename... Params>
class TMulticastDelegate
{
public:
    using FunctionType = std::function<void(Params...)>;

    TMulticastDelegate() : m_broadcastDepth(0), m_pendingRemovals(false) {}
    TMulticastDelegate(const TMulticastDelegate&) = delete;
    TMulticastDelegate& operator=(const TMulticastDelegate&) = delete;
    TMulticastDelegate(TMulticastDelegate&&) = default;
    TMulticastDelegate& operator=(TMulticastDelegate&&) = default;

    // Binds a member function (const or non-const) of obj. obj must outlive the binding.
    template <typename UserClass, typename MethodType>
    FDelegateHandle Add(UserClass* obj, MethodType method)
    {
        static_assert(std::is_member_function_pointer<MethodType>::value, "Add expects a member function pointer");
        static_assert(std::is_invocable<MethodType, UserClass*, Params...>::value,
            "Member function cannot be called on this object with the delegate's parameters");
        return AddInternal([obj, method](Params... params) { (obj->*method)(params...); }, obj);
    }

    // Binds any callable (lambda, functor, free function) taking the delegate's parameters.
    template <typename FunctorType>
    FDelegateHandle AddLambda(FunctorType&& functor)
    {
        static_assert(std::is_invocable<std::decay_t<FunctorType>&, Params...>::value,
            "Callable cannot be called with the delegate's parameters");
        return AddInternal(FunctionType(std::forward<FunctorType>(functor)), nullptr);
    }

    // Unbinds the binding identified by handle. Returns false if it was not bound here.
    bool Remove(FDelegateHandle handle)
    {
        if (!handle.IsValid())
            return false;

        for (std::size_t i = 0; i < m_entries.size(); ++i)
        {
            if (m_entries[i]->handle == handle)
            {
                RemoveAt(i);
                return true;
            }
        }
        return false;
    }

    // Unbinds every member function binding made with Add(obj, ...). Returns the number removed.
    uint32 RemoveAll(const void* obj)
    {
        if (!obj)
            return 0;

        uint32 removed = 0;
        for (std::size_t i = m_entries.size(); i > 0; --i)
        {
            if (m_entries[i - 1]->owner == obj && m_entries[i - 1]->handle.IsValid())
            {
                RemoveAt(i - 1);
                ++removed;
            }
        }
        return removed;
    }

    void Clear()
    {
        for (std::size_t i = m_entries.size(); i > 0; --i)
            RemoveAt(i - 1);
    }

    bool IsBound() const
    {
        for (const auto& entry : m_entries)
            if (entry->handle.IsValid())
                return true;
        return false;
    }

    bool IsBoundToObject(const void* obj) const
    {
        for (const auto& entry : m_entries)
            if (obj && entry->owner == obj && entry->handle.IsValid())
                return true;
        return false;
    }

    // Calls every subscriber bound when the broadcast started, in binding order.
    void Broadcast(Params... params) const
    {
        BroadcastScope scope(*this);
        std::size_t const count = m_entries.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            // entries are heap allocated, so the reference stays valid if Add reallocates m_entries
            Entry const& entry = *m_entries[i];
            if (entry.handle.IsValid())
                entry.function(params...);
        }
    }

private:
    struct Entry
    {
        FDelegateHandle handle;   // reset when removed during a broadcast
        FunctionType function;
        const void* owner;
    };

    struct BroadcastScope
    {
        explicit BroadcastScope(const TMulticastDelegate& delegate) : m_delegate(delegate) { ++m_delegate.m_broadcastDepth; }
        ~BroadcastScope()
        {
            if (--m_delegate.m_broadcastDepth == 0 && m_delegate.m_pendingRemovals)
                m_delegate.CompactEntries();
        }

        const TMulticastDelegate& m_delegate;
    };

    FDelegateHandle AddInternal(FunctionType&& function, const void* owner)
    {
        FDelegateHandle handle = FDelegateHandle::GenerateNewHandle();
        m_entries.push_back(std::make_unique<Entry>(Entry{handle, std::move(function), owner}));
        return handle;
    }

    void RemoveAt(std::size_t index) const
    {
        if (m_broadcastDepth > 0)
        {
            // the function may be executing right now - only mark it, CompactEntries frees it later
            m_entries[index]->handle.Reset();
            m_pendingRemovals = true;
        }
        else
            m_entries.erase(m_entries.begin() + index);
    }

    void CompactEntries() const
    {
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
            [](const std::unique_ptr<Entry>& entry) { return !entry->handle.IsValid(); }), m_entries.end());
        m_pendingRemovals = false;
    }

    mutable std::vector<std::unique_ptr<Entry>> m_entries;
    mutable uint32 m_broadcastDepth;
    mutable bool m_pendingRemovals;
};

// Declares FDelegateType as a multicast delegate class. Use with a trailing semicolon.
#define MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, ...) \
    class FDelegateType : public TMulticastDelegate<__VA_ARGS__> {}

#define DECLARE_MULTICAST_DELEGATE(FDelegateType) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType)
#define DECLARE_MULTICAST_DELEGATE_OneParam(FDelegateType, Param1Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type)
#define DECLARE_MULTICAST_DELEGATE_TwoParams(FDelegateType, Param1Type, Param2Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type)
#define DECLARE_MULTICAST_DELEGATE_ThreeParams(FDelegateType, Param1Type, Param2Type, Param3Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type, Param3Type)
#define DECLARE_MULTICAST_DELEGATE_FourParams(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type)
#define DECLARE_MULTICAST_DELEGATE_FiveParams(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type)
#define DECLARE_MULTICAST_DELEGATE_SixParams(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type)
#define DECLARE_MULTICAST_DELEGATE_SevenParams(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type, Param7Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type, Param7Type)
#define DECLARE_MULTICAST_DELEGATE_EightParams(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type, Param7Type, Param8Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type, Param7Type, Param8Type)
#define DECLARE_MULTICAST_DELEGATE_NineParams(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type, Param7Type, Param8Type, Param9Type) \
    MANGOS_DECLARE_MULTICAST_DELEGATE_IMPL(FDelegateType, Param1Type, Param2Type, Param3Type, Param4Type, Param5Type, Param6Type, Param7Type, Param8Type, Param9Type)

#endif
