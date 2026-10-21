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

#ifndef MANGOS_AUTOMATIONTEST_TESTWORLD_H
#define MANGOS_AUTOMATIONTEST_TESTWORLD_H

#include "Common.h"
#include "Entities/ObjectGuid.h"

#include <functional>

class Map;
class Creature;
struct CreatureInfo;

// world helpers for the automation tests (world thread, between two World::Update calls)
namespace MangosTestWorld
{
    // "Testing" (Map.dbc 13): a continent type map without any terrain file
    constexpr uint32 FlatMapId = 13;

    // the map of mapId (non instanceable) made a flat ground at height, with no vmap / mmap: line of sight
    // everywhere, straight paths. Created on first use and kept for the whole run. nullptr when the map is unknown.
    Map* CreateFlatMap(uint32 mapId = FlatMapId, float height = 0.0f);

    // the creature template of the lowest entry matching predicate, 0 when none
    uint32 FindCreatureEntry(std::function<bool(CreatureInfo const&)> const& predicate);

    // a plain fighter for the level: non elite beast or humanoid, hostile to players, attackable, no npc flags.
    // Templates of exactly that level are preferred to ranges including it. 0 when none.
    uint32 FindNormalHostileCreatureEntry(uint32 level);

    // a temporary creature of entry (despawned only by Despawn), at the level when given
    Creature* SpawnCreature(Map* map, uint32 entry, float x, float y, float z, float orientation, uint32 level = 0);

    // removes a creature spawned by SpawnCreature, alive or dead
    void Despawn(Map* map, ObjectGuid guid);
}

#endif
