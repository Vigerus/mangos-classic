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

#include "TestWorld.h"

#include "Entities/Creature.h"
#include "Entities/TemporarySpawn.h"
#include "Globals/ObjectMgr.h"
#include "Maps/GridMap.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "Server/DBCStores.h"
#include "Server/SQLStorages.h"

Map* MangosTestWorld::CreateFlatMap(uint32 mapId, float height)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry || entry->Instanceable())
        return nullptr;

    // the terrain must be flat before the map loads any grid
    TerrainInfo* terrain = sTerrainMgr.LoadTerrain(mapId);
    if (!terrain->IsFlatPlane())
        terrain->SetFlatPlane(height);

    return sMapMgr.CreateMap(mapId, nullptr);
}

uint32 MangosTestWorld::FindCreatureEntry(std::function<bool(CreatureInfo const&)> const& predicate)
{
    for (auto itr = sCreatureStorage.getDataBegin<CreatureInfo>(); itr < sCreatureStorage.getDataEnd<CreatureInfo>(); ++itr)
        if (CreatureInfo const* info = *itr)
            if (predicate(*info))
                return info->Entry;
    return 0;
}

uint32 MangosTestWorld::FindNormalHostileCreatureEntry(uint32 level)
{
    auto isFighter = [](CreatureInfo const& info)
    {
        if (info.Rank != CREATURE_ELITE_NORMAL || info.NpcFlags || !info.MinLevelHealth)
            return false;
        if (info.CreatureType != CREATURE_TYPE_BEAST && info.CreatureType != CREATURE_TYPE_HUMANOID)
            return false;
        if (info.UnitFlags & (UNIT_FLAG_SPAWNING | UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_UNTARGETABLE | UNIT_FLAG_PACIFIED))
            return false;
        if (info.ExtraFlags & (CREATURE_EXTRA_FLAG_CIVILIAN | CREATURE_EXTRA_FLAG_NO_AGGRO_ON_SIGHT))
            return false;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info.Faction);
        return faction && faction->IsHostileToPlayers();
    };

    if (uint32 entry = FindCreatureEntry([&](CreatureInfo const& info) { return info.MinLevel == level && info.MaxLevel == level && isFighter(info); }))
        return entry;
    return FindCreatureEntry([&](CreatureInfo const& info) { return info.MinLevel <= level && info.MaxLevel >= level && isFighter(info); });
}

Creature* MangosTestWorld::SpawnCreature(Map* map, uint32 entry, float x, float y, float z, float orientation, uint32 level)
{
    if (!map)
        return nullptr;

    TempSpawnSettings settings(nullptr, entry, x, y, z, orientation, TEMPSPAWN_MANUAL_DESPAWN, 0);
    settings.level = level;
    return WorldObject::SummonCreature(settings, map);
}

void MangosTestWorld::Despawn(Map* map, ObjectGuid guid)
{
    if (!map || !guid)
        return;

    if (Creature* creature = map->GetCreature(guid))
    {
        if (creature->IsTemporarySummon())
            static_cast<TemporarySpawn*>(creature)->UnSummon();
        else
            creature->AddObjectToRemoveList();
    }
}
