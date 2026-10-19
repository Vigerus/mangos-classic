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

#ifndef MANGOS_WORLDSUBSYSTEM_H
#define MANGOS_WORLDSUBSYSTEM_H

#include "Platform/Define.h"

/*
 * A singleton-like service owned by World, reachable from everywhere through
 *
 *   if (MySubsystem* subsystem = sWorld.GetSubsystem<MySubsystem>())
 *       ...
 *
 * Subsystems are registered with World::RegisterSubsystem<T>() while the server starts (World constructor),
 * before any map thread runs, and destroyed with World in reverse registration order.
 */
class WorldSubsystem
{
    public:
        virtual ~WorldSubsystem() = default;

        // called right after the subsystem was registered
        virtual void Initialize() {}
        // called before the subsystem is destroyed (World destructor)
        virtual void Deinitialize() {}
        // called from World::Update on the world thread
        virtual void Update(uint32 /*diff*/) {}
};

// dense index per subsystem class, used by World to look subsystems up without hashing
uint32 GenerateWorldSubsystemTypeIndex();

template <class T>
uint32 GetWorldSubsystemTypeIndex()
{
    static uint32 const index = GenerateWorldSubsystemTypeIndex();
    return index;
}

#endif
