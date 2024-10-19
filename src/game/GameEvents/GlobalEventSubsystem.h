#pragma once

#include "Entities/Player.h"

#include <mutex>
#include <map>
#include <functional>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>

//DECLARE_MULTICAST_DELEGATE_TwoParams(OnItemCreated, )
template <typename... Ts>
class MulticastDelegate
{
public:
    boost::uuids::uuid Add(std::function<void(Ts...)> fct)
    {
        std::lock_guard<std::mutex> lock(mtx);
        boost::uuids::uuid new_uuid = boost::uuids::random_generator()();

        eventmap.insert(std::make_pair(new_uuid, fct));

        return new_uuid;
    }

    void Remove(boost::uuids::uuid subbed_uuid)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = eventmap.find(subbed_uuid);

        if (it != eventmap.end())
        {
            eventmap.erase(it);
        }
    }

    void Broadcast(Ts... args)
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& el : eventmap)
        {
            el.second(args...);
        }
    }

private:
    std::mutex mtx;
    std::map<boost::uuids::uuid, std::function<void(Ts...)>> eventmap;
};

using OnItemCreated = MulticastDelegate<Player*,uint32>;
using OnAuraApplied = MulticastDelegate<Unit*,uint32>;

class GlobalEventSubsystem
{
public:
    GlobalEventSubsystem();
    virtual ~GlobalEventSubsystem();
    static GlobalEventSubsystem& instance()
    {
        static GlobalEventSubsystem instance;
        return instance;
    };

    OnItemCreated& GetOnItemCreatedDelegate()
    {
        return onItemCreatedDelegate;
    };

    OnAuraApplied& GetOnAuraAppliedDelegate()
    {
        return onAuraAppliedDelegate;
    };

private:
    OnItemCreated onItemCreatedDelegate;
    OnAuraApplied onAuraAppliedDelegate;
};