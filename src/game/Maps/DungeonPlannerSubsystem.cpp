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

#include "Maps/DungeonPlannerSubsystem.h"
#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/CreatureLinkingMgr.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Maps/SpawnGroupDefines.h"
#include "Server/DBCStores.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>

namespace
{
    // spawns this close (and at about the same height) are one pack
    constexpr float PACK_LINK_RANGE = 10.0f;
    constexpr float PACK_LINK_HEIGHT = 5.0f;
    // names of the entries in a generated name
    constexpr size_t NAME_ENTRIES = 3;
    // addon messages are limited to 255 chars
    constexpr size_t CHUNK_SIZE = 220;
    constexpr size_t MAX_PULLS = 500;

    struct UnionFind
    {
        std::vector<size_t> parent;

        explicit UnionFind(size_t size) : parent(size)
        {
            for (size_t i = 0; i < size; ++i)
                parent[i] = i;
        }

        size_t Find(size_t i)
        {
            while (parent[i] != i)
            {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        }

        void Join(size_t a, size_t b)
        {
            a = Find(a);
            b = Find(b);
            if (a != b)
                parent[std::max(a, b)] = std::min(a, b);
        }
    };

    // creatures the bots fight: no critters, triggers (uninteractible, invisible), creatures friendly to players
    bool IsFought(CreatureInfo const* info)
    {
        if (!info || info->CreatureType == CREATURE_TYPE_CRITTER)
            return false;
        if ((info->UnitFlags & UNIT_FLAG_UNINTERACTIBLE) || (info->ExtraFlags & CREATURE_EXTRA_FLAG_INVISIBLE))
            return false;
        if (FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->Faction))
            if (faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER)
                return false;
        return true;
    }

    // walking distance estimate: height counts more (another floor is far)
    float WalkDistanceSq(float x1, float y1, float z1, float x2, float y2, float z2)
    {
        float const dx = x1 - x2, dy = y1 - y2, dz = (z1 - z2) * 3.0f;
        return dx * dx + dy * dy + dz * dz;
    }

    std::vector<uint32> EntriesOf(std::vector<uint32> const& guids)
    {
        std::vector<uint32> entries;
        for (uint32 guid : guids)
            if (CreatureData const* data = sObjectMgr.GetCreatureData(guid))
                entries.push_back(data->id);
        return entries;
    }

    std::vector<uint32> ParseGuids(std::string const& text)
    {
        std::vector<uint32> guids;
        std::istringstream in(text);
        std::string part;
        while (std::getline(in, part, ','))
            if (uint32 const guid = uint32(std::strtoul(part.c_str(), nullptr, 10)))
                guids.push_back(guid);
        return guids;
    }

    std::string JoinGuids(std::vector<uint32> const& guids)
    {
        std::ostringstream out;
        for (size_t i = 0; i < guids.size(); ++i)
            out << (i ? "," : "") << guids[i];
        return out.str();
    }

    // "x y z;x y z" (database) / "x,y,z;x,y,z" (addon)
    std::string JoinPoints(std::vector<DungeonPlanPoint> const& points, char sep)
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1);
        for (size_t i = 0; i < points.size(); ++i)
            out << (i ? ";" : "") << points[i].x << sep << points[i].y << sep << points[i].z;
        return out.str();
    }

    std::vector<DungeonPlanPoint> ParsePoints(std::string const& text)
    {
        std::vector<DungeonPlanPoint> points;
        std::istringstream in(text);
        std::string part;
        while (std::getline(in, part, ';'))
        {
            std::replace(part.begin(), part.end(), ',', ' ');
            std::istringstream values(part);
            DungeonPlanPoint point;
            if (values >> point.x >> point.y >> point.z)
                points.push_back(point);
        }
        return points;
    }

    std::string PointText(bool has, DungeonPlanPoint const& point)
    {
        return has ? JoinPoints({ point }, ',') : std::string();
    }

    // names go into '^' / '~' separated records
    std::string Clean(std::string text)
    {
        std::replace(text.begin(), text.end(), '^', ' ');
        std::replace(text.begin(), text.end(), '~', ' ');
        return text;
    }

    // the pulls' ids (lowest creature) and names (from their creatures), creatures sorted, empty pulls dropped
    void Normalize(DungeonPreset& preset)
    {
        std::vector<DungeonPlanPull> pulls;
        for (DungeonPlanPull& pull : preset.pulls)
        {
            std::sort(pull.guids.begin(), pull.guids.end());
            pull.guids.erase(std::unique(pull.guids.begin(), pull.guids.end()), pull.guids.end());
            if (pull.guids.empty())
                continue;
            pull.id = pull.guids.front();
            pull.name = DungeonPlannerSubsystem::MakeName(EntriesOf(pull.guids));
            pulls.push_back(std::move(pull));
        }
        preset.pulls = std::move(pulls);
    }
}

// ---------------------------------------------------------------- delegates

bool DungeonPlannerSubsystem::Remove(ObjectGuid self, FDelegateHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_delegateLock);
    auto itr = m_delegates.find(self);
    if (itr == m_delegates.end())
        return false;
    bool const removed = itr->second.Remove(handle);
    if (!itr->second.IsBound())
        m_delegates.erase(itr);
    return removed;
}

uint32 DungeonPlannerSubsystem::RemoveAll(ObjectGuid self, const void* obj)
{
    std::lock_guard<std::recursive_mutex> lock(m_delegateLock);
    auto itr = m_delegates.find(self);
    if (itr == m_delegates.end())
        return 0;
    uint32 const removed = itr->second.RemoveAll(obj);
    if (!itr->second.IsBound())
        m_delegates.erase(itr);
    return removed;
}

void DungeonPlannerSubsystem::BroadcastPlanChanged(ObjectGuid self, ObjectGuid selector, uint32 mapId)
{
    std::lock_guard<std::recursive_mutex> lock(m_delegateLock);
    auto itr = m_delegates.find(self);
    // the callbacks only flag the change: they do not (un)subscribe while it runs
    if (itr != m_delegates.end())
        itr->second.Broadcast(self, selector, mapId);
}

void DungeonPlannerSubsystem::BroadcastPlanChanged(Player* selector, uint32 mapId)
{
    ObjectGuid const guid = selector->GetObjectGuid();
    BroadcastPlanChanged(guid, guid, mapId);
    if (Group* group = selector->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->getSource())
                if (member != selector)
                    BroadcastPlanChanged(member->GetObjectGuid(), guid, mapId);
}

// ---------------------------------------------------------------- packs

std::shared_ptr<const std::vector<DungeonPack>> DungeonPlannerSubsystem::GetPacks(uint32 mapId)
{
    {
        std::lock_guard<std::mutex> lock(m_packsLock);
        auto itr = m_packs.find(mapId);
        if (itr != m_packs.end())
            return itr->second;
    }

    // built outside the lock: the creature data is read only at runtime
    auto packs = std::make_shared<const std::vector<DungeonPack>>(GeneratePacks(mapId));
    std::lock_guard<std::mutex> lock(m_packsLock);
    return m_packs.emplace(mapId, packs).first->second;
}

std::string DungeonPlannerSubsystem::MakeName(std::vector<uint32> const& entries)
{
    std::map<uint32, uint32> counts;
    for (uint32 entry : entries)
        ++counts[entry];

    // the highest rank (boss, elite) first, then the most frequent
    std::vector<std::pair<uint32, uint32>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](std::pair<uint32, uint32> const& a, std::pair<uint32, uint32> const& b)
    {
        CreatureInfo const* infoA = ObjectMgr::GetCreatureTemplate(a.first);
        CreatureInfo const* infoB = ObjectMgr::GetCreatureTemplate(b.first);
        uint32 const rankA = infoA ? infoA->Rank : 0, rankB = infoB ? infoB->Rank : 0;
        if (rankA != rankB)
            return rankA > rankB;
        return a.second > b.second;
    });

    std::ostringstream out;
    for (size_t i = 0; i < sorted.size() && i < NAME_ENTRIES; ++i)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(sorted[i].first);
        out << (i ? ", " : "") << (info && info->Name ? std::string(info->Name) : "creature " + std::to_string(sorted[i].first));
        if (sorted[i].second > 1)
            out << " x" << sorted[i].second;
    }
    if (sorted.size() > NAME_ENTRIES)
        out << ", ...";
    return Clean(out.str());
}

bool DungeonPlannerSubsystem::IsSpawnAlive(Map* map, uint32 dbGuid)
{
    if (Creature* creature = map->GetCreature(dbGuid))     // prefers a living one
        return creature->IsAlive();

    CreatureData const* data = sObjectMgr.GetCreatureData(dbGuid);
    if (!data)
        return false;
    // not in the map: killed and removed or not spawned while its grid is loaded, else waiting for the grid
    if (map->IsLoaded(data->posX, data->posY))
        return false;

    MapPersistentState* state = map->GetPersistentState();
    return !state || state->GetCreatureRespawnTime(dbGuid) <= time(nullptr);
}

std::vector<DungeonPack> DungeonPlannerSubsystem::GeneratePacks(uint32 mapId)
{
    std::vector<DungeonPackSpawn> spawns;
    auto worker = [&](CreatureDataPair const& pair) -> bool
    {
        CreatureData const& data = pair.second;
        if (data.mapid == mapId && IsFought(ObjectMgr::GetCreatureTemplate(data.id)))
            spawns.push_back({ pair.first, data.id, data.posX, data.posY, data.posZ });
        return false;
    };
    sObjectMgr.DoCreatureData(worker);

    std::unordered_map<uint32, size_t> byGuid;
    for (size_t i = 0; i < spawns.size(); ++i)
        byGuid[spawns[i].dbGuid] = i;

    UnionFind sets(spawns.size());

    // close to each other
    for (size_t i = 0; i < spawns.size(); ++i)
        for (size_t j = i + 1; j < spawns.size(); ++j)
        {
            float const dx = spawns[i].x - spawns[j].x, dy = spawns[i].y - spawns[j].y;
            if (dx * dx + dy * dy <= PACK_LINK_RANGE * PACK_LINK_RANGE && std::abs(spawns[i].z - spawns[j].z) <= PACK_LINK_HEIGHT)
                sets.Join(i, j);
        }

    // one spawn group
    if (std::shared_ptr<SpawnGroupEntryContainer> groups = sObjectMgr.GetSpawnGroupContainer())
    {
        std::unordered_map<uint32, size_t> firstOfGroup;
        for (size_t i = 0; i < spawns.size(); ++i)
        {
            auto itr = groups->spawnGroupByGuidMap.find(std::make_pair(spawns[i].dbGuid, uint32(TYPEID_UNIT)));
            if (itr == groups->spawnGroupByGuidMap.end() || !itr->second)
                continue;
            auto first = firstOfGroup.emplace(itr->second->Id, i).first;
            sets.Join(first->second, i);
        }
    }

    // linked to a master (creature_linking): the master by its guid, else the nearest of its entry
    for (size_t i = 0; i < spawns.size(); ++i)
    {
        CreatureLinkingInfo const* link = sCreatureLinkingMgr.GetLinkedTriggerInformation(spawns[i].entry, spawns[i].dbGuid, mapId);
        if (!link)
            continue;
        if (link->masterDBGuid)
        {
            auto itr = byGuid.find(link->masterDBGuid);
            if (itr != byGuid.end())
                sets.Join(itr->second, i);
            continue;
        }

        size_t nearest = spawns.size();
        float nearestDistance = 0.0f;
        for (size_t j = 0; j < spawns.size(); ++j)
        {
            if (spawns[j].entry != link->masterId)
                continue;
            float const distance = WalkDistanceSq(spawns[i].x, spawns[i].y, spawns[i].z, spawns[j].x, spawns[j].y, spawns[j].z);
            if (nearest == spawns.size() || distance < nearestDistance)
            {
                nearest = j;
                nearestDistance = distance;
            }
        }
        if (nearest != spawns.size())
            sets.Join(nearest, i);
    }

    std::map<size_t, DungeonPack> byRoot;
    for (size_t i = 0; i < spawns.size(); ++i)
        byRoot[sets.Find(i)].spawns.push_back(spawns[i]);

    std::vector<DungeonPack> unordered;
    for (auto& [root, pack] : byRoot)
    {
        std::vector<uint32> entries;
        pack.id = pack.spawns.front().dbGuid;
        for (DungeonPackSpawn const& spawn : pack.spawns)
        {
            pack.id = std::min(pack.id, spawn.dbGuid);
            pack.x += spawn.x;
            pack.y += spawn.y;
            pack.z += spawn.z;
            entries.push_back(spawn.entry);
        }
        pack.x /= pack.spawns.size();
        pack.y /= pack.spawns.size();
        pack.z /= pack.spawns.size();
        pack.name = MakeName(entries);
        unordered.push_back(std::move(pack));
    }

    // the walking order: from the entrance (the teleport target into the map), always the nearest pack left
    float x = 0.0f, y = 0.0f, z = 0.0f;
    AreaTrigger const* entrance = sObjectMgr.GetMapEntranceTrigger(mapId);
    if (entrance && entrance->target_mapId == mapId)
    {
        x = entrance->target_X;
        y = entrance->target_Y;
        z = entrance->target_Z;
    }
    else if (!unordered.empty())
    {
        x = unordered.front().x;
        y = unordered.front().y;
        z = unordered.front().z;
    }

    std::vector<DungeonPack> packs;
    std::vector<bool> taken(unordered.size(), false);
    for (size_t n = 0; n < unordered.size(); ++n)
    {
        size_t best = unordered.size();
        float bestDistance = 0.0f;
        for (size_t i = 0; i < unordered.size(); ++i)
        {
            if (taken[i])
                continue;
            float const distance = WalkDistanceSq(x, y, z, unordered[i].x, unordered[i].y, unordered[i].z);
            if (best == unordered.size() || distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
        }
        taken[best] = true;
        x = unordered[best].x;
        y = unordered[best].y;
        z = unordered[best].z;
        packs.push_back(std::move(unordered[best]));
    }

    sLog.outString("DungeonPlannerSubsystem: map %u: %u packs from %u creatures", mapId, uint32(packs.size()), uint32(spawns.size()));
    return packs;
}

// ---------------------------------------------------------------- presets

void DungeonPlannerSubsystem::CreateTables()
{
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `dungeon_planner_presets` ("
        "`id` int(10) unsigned NOT NULL COMMENT 'preset id',"
        "`map` int(10) unsigned NOT NULL COMMENT 'dungeon map id',"
        "`name` varchar(100) NOT NULL DEFAULT '',"
        "PRIMARY KEY (`id`), KEY `map` (`map`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC COMMENT='Bot Dungeon Planner presets'");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `dungeon_planner_pulls` ("
        "`preset` int(10) unsigned NOT NULL COMMENT 'dungeon_planner_presets.id',"
        "`pull` int(10) unsigned NOT NULL COMMENT 'order in the preset, from 0',"
        "`creatures` text NOT NULL COMMENT 'creature db guids, comma separated',"
        "`has_start` tinyint(3) unsigned NOT NULL DEFAULT '0',"
        "`start_x` float NOT NULL DEFAULT '0', `start_y` float NOT NULL DEFAULT '0', `start_z` float NOT NULL DEFAULT '0',"
        "`has_cleared` tinyint(3) unsigned NOT NULL DEFAULT '0',"
        "`cleared_x` float NOT NULL DEFAULT '0', `cleared_y` float NOT NULL DEFAULT '0', `cleared_z` float NOT NULL DEFAULT '0',"
        "`path` text NOT NULL COMMENT 'waypoints to the start position: \"x y z;x y z\"',"
        "PRIMARY KEY (`preset`, `pull`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC COMMENT='Bot Dungeon Planner pulls of the presets'");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `dungeon_planner_selections` ("
        "`player` int(10) unsigned NOT NULL COMMENT 'character guid',"
        "`map` int(10) unsigned NOT NULL COMMENT 'dungeon map id',"
        "`preset` int(10) unsigned NOT NULL COMMENT 'dungeon_planner_presets.id',"
        "PRIMARY KEY (`player`, `map`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC COMMENT='Bot Dungeon Planner preset of a character per dungeon'");
}

// with m_presetLock held
void DungeonPlannerSubsystem::LoadPresets()
{
    if (m_loaded)
        return;
    m_loaded = true;
    CreateTables();

    std::map<uint32, DungeonPreset> presets;
    if (auto result = CharacterDatabase.Query("SELECT `id`, `map`, `name` FROM `dungeon_planner_presets`"))
    {
        do
        {
            Field* fields = result->Fetch();
            DungeonPreset& preset = presets[fields[0].GetUInt32()];
            preset.id = fields[0].GetUInt32();
            preset.mapId = fields[1].GetUInt32();
            preset.name = Clean(fields[2].GetCppString());
        }
        while (result->NextRow());
    }

    if (auto result = CharacterDatabase.Query("SELECT `preset`, `creatures`, `has_start`, `start_x`, `start_y`, `start_z`, `has_cleared`, `cleared_x`, `cleared_y`, `cleared_z`, `path` FROM `dungeon_planner_pulls` ORDER BY `preset`, `pull`"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto itr = presets.find(fields[0].GetUInt32());
            if (itr == presets.end())
                continue;
            DungeonPlanPull pull;
            pull.guids = ParseGuids(fields[1].GetCppString());
            pull.hasStart = fields[2].GetUInt8() != 0;
            pull.start = { fields[3].GetFloat(), fields[4].GetFloat(), fields[5].GetFloat() };
            pull.hasCleared = fields[6].GetUInt8() != 0;
            pull.cleared = { fields[7].GetFloat(), fields[8].GetFloat(), fields[9].GetFloat() };
            pull.path = ParsePoints(fields[10].GetCppString());
            itr->second.pulls.push_back(std::move(pull));
        }
        while (result->NextRow());
    }

    for (auto& [id, preset] : presets)
    {
        Normalize(preset);
        m_presets[id] = std::make_shared<DungeonPreset const>(std::move(preset));
    }

    if (auto result = CharacterDatabase.Query("SELECT `player`, `map`, `preset` FROM `dungeon_planner_selections`"))
    {
        do
        {
            Field* fields = result->Fetch();
            if (m_presets.count(fields[2].GetUInt32()))
                m_selections[{ ObjectGuid(HIGHGUID_PLAYER, fields[0].GetUInt32()), fields[1].GetUInt32() }] = fields[2].GetUInt32();
        }
        while (result->NextRow());
    }

    sLog.outString("DungeonPlannerSubsystem: %u presets, %u selections", uint32(m_presets.size()), uint32(m_selections.size()));
}

std::vector<std::pair<uint32, std::string>> DungeonPlannerSubsystem::GetPresets(uint32 mapId)
{
    std::lock_guard<std::mutex> lock(m_presetLock);
    LoadPresets();
    std::vector<std::pair<uint32, std::string>> list;
    for (auto const& [id, preset] : m_presets)
        if (preset->mapId == mapId)
            list.emplace_back(id, preset->name);
    std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.second < b.second; });
    return list;
}

std::shared_ptr<DungeonPreset const> DungeonPlannerSubsystem::GetPreset(uint32 presetId)
{
    std::lock_guard<std::mutex> lock(m_presetLock);
    LoadPresets();
    auto itr = m_presets.find(presetId);
    return itr != m_presets.end() ? itr->second : nullptr;
}

uint32 DungeonPlannerSubsystem::SavePreset(DungeonPreset preset)
{
    std::lock_guard<std::mutex> lock(m_presetLock);
    LoadPresets();

    if (!preset.id || !m_presets.count(preset.id))
        preset.id = m_presets.empty() ? 1 : m_presets.rbegin()->first + 1;
    preset.name = Clean(preset.name.empty() ? "plan" : preset.name.substr(0, 100));
    Normalize(preset);

    std::string name = preset.name;
    CharacterDatabase.escape_string(name);
    CharacterDatabase.DirectPExecute("REPLACE INTO `dungeon_planner_presets` (`id`, `map`, `name`) VALUES ('%u', '%u', '%s')", preset.id, preset.mapId, name.c_str());
    CharacterDatabase.DirectPExecute("DELETE FROM `dungeon_planner_pulls` WHERE `preset` = '%u'", preset.id);
    for (size_t i = 0; i < preset.pulls.size(); ++i)
    {
        DungeonPlanPull const& pull = preset.pulls[i];
        CharacterDatabase.DirectPExecute("INSERT INTO `dungeon_planner_pulls` (`preset`, `pull`, `creatures`, `has_start`, `start_x`, `start_y`, `start_z`, `has_cleared`, `cleared_x`, `cleared_y`, `cleared_z`, `path`) "
            "VALUES ('%u', '%u', '%s', '%u', '%f', '%f', '%f', '%u', '%f', '%f', '%f', '%s')",
            preset.id, uint32(i), JoinGuids(pull.guids).c_str(), pull.hasStart ? 1 : 0, pull.start.x, pull.start.y, pull.start.z,
            pull.hasCleared ? 1 : 0, pull.cleared.x, pull.cleared.y, pull.cleared.z, JoinPoints(pull.path, ' ').c_str());
    }

    uint32 const id = preset.id;
    m_presets[id] = std::make_shared<DungeonPreset const>(std::move(preset));
    return id;
}

void DungeonPlannerSubsystem::DeletePreset(uint32 presetId)
{
    std::lock_guard<std::mutex> lock(m_presetLock);
    LoadPresets();
    m_presets.erase(presetId);
    for (auto itr = m_selections.begin(); itr != m_selections.end();)
    {
        if (itr->second == presetId)
            itr = m_selections.erase(itr);
        else
            ++itr;
    }

    CharacterDatabase.DirectPExecute("DELETE FROM `dungeon_planner_presets` WHERE `id` = '%u'", presetId);
    CharacterDatabase.DirectPExecute("DELETE FROM `dungeon_planner_pulls` WHERE `preset` = '%u'", presetId);
    CharacterDatabase.DirectPExecute("DELETE FROM `dungeon_planner_selections` WHERE `preset` = '%u'", presetId);
#ifdef ENABLE_PLAYERBOTS
    // the bots' spots for the preset's pulls (PlayerbotDungeonPositions)
    CharacterDatabase.DirectPExecute("DELETE FROM `ai_playerbot_dungeon_positions` WHERE `preset` = '%u'", presetId);
#endif
}

uint32 DungeonPlannerSubsystem::GetSelectedPreset(ObjectGuid player, uint32 mapId)
{
    std::lock_guard<std::mutex> lock(m_presetLock);
    LoadPresets();
    auto itr = m_selections.find({ player, mapId });
    return itr != m_selections.end() ? itr->second : 0;
}

void DungeonPlannerSubsystem::SelectPreset(Player* player, uint32 mapId, uint32 presetId)
{
    {
        std::lock_guard<std::mutex> lock(m_presetLock);
        LoadPresets();
        auto itr = m_presets.find(presetId);
        if (presetId && (itr == m_presets.end() || itr->second->mapId != mapId))
            return;

        uint32 const guid = player->GetGUIDLow();
        if (presetId)
        {
            m_selections[{ player->GetObjectGuid(), mapId }] = presetId;
            CharacterDatabase.DirectPExecute("REPLACE INTO `dungeon_planner_selections` (`player`, `map`, `preset`) VALUES ('%u', '%u', '%u')", guid, mapId, presetId);
        }
        else
        {
            m_selections.erase({ player->GetObjectGuid(), mapId });
            CharacterDatabase.DirectPExecute("DELETE FROM `dungeon_planner_selections` WHERE `player` = '%u' AND `map` = '%u'", guid, mapId);
        }
        // the same preset again (a saved new version of it): the run goes on, Run checks its active pull
        auto run = m_runs.find(player->GetObjectGuid());
        if (run != m_runs.end() && run->second.mapId == mapId && run->second.presetId != presetId)
            m_runs.erase(run);
    }
    BroadcastPlanChanged(player, mapId);
}

// with m_presetLock held
DungeonPlannerSubsystem::RunState& DungeonPlannerSubsystem::Run(Player* player, Map* map, DungeonPreset const& preset)
{
    RunState& run = m_runs[player->GetObjectGuid()];
    if (run.mapId == map->GetId() && run.instanceId == map->GetInstanceId() && run.presetId == preset.id)
    {
        if (run.active >= 0 && run.active < int32(preset.pulls.size()) && preset.pulls[run.active].id == run.pullId)
            return run;
        // a new version of the preset: its active pull where it is now, if it still has it
        for (size_t i = 0; i < preset.pulls.size(); ++i)
        {
            if (preset.pulls[i].id == run.pullId)
            {
                run.active = int32(i);
                return run;
            }
        }
    }

    run.mapId = map->GetId();
    run.instanceId = map->GetInstanceId();
    run.presetId = preset.id;
    // the first pull that is not cleared, else the last
    int32 active = int32(preset.pulls.size()) - 1;
    for (size_t i = 0; i < preset.pulls.size(); ++i)
    {
        bool alive = false;
        for (uint32 guid : preset.pulls[i].guids)
            alive = alive || IsSpawnAlive(map, guid);
        if (alive)
        {
            active = int32(i);
            break;
        }
    }
    SetActive(run, preset, active);
    return run;
}

// with m_presetLock held
DungeonPlannerSubsystem::RunState* DungeonPlannerSubsystem::SelectedRun(Player* player, DungeonPreset const*& preset)
{
    Map* map = player->GetMap();
    LoadPresets();
    auto selection = m_selections.find({ player->GetObjectGuid(), map->GetId() });
    if (selection == m_selections.end() || !map->IsDungeon())
        return nullptr;
    auto itr = m_presets.find(selection->second);
    if (itr == m_presets.end() || itr->second->pulls.empty())
        return nullptr;
    preset = itr->second.get();
    return &Run(player, map, *preset);
}

void DungeonPlannerSubsystem::SetActive(RunState& run, DungeonPreset const& preset, int32 active)
{
    run.active = std::max(0, std::min(int32(preset.pulls.size()) - 1, active));
    run.pullId = preset.pulls[run.active].id;
}

bool DungeonPlannerSubsystem::GetActivePull(Player* player, Map* map, DungeonActivePull& pull)
{
    if (!player || !map || !map->IsDungeon())
        return false;

    std::lock_guard<std::mutex> lock(m_presetLock);
    LoadPresets();
    auto selection = m_selections.find({ player->GetObjectGuid(), map->GetId() });
    if (selection == m_selections.end())
        return false;
    auto itr = m_presets.find(selection->second);
    if (itr == m_presets.end())
        return false;

    DungeonPreset const& preset = *itr->second;
    pull.presetId = preset.id;
    pull.presetName = preset.name;
    // a preset without pulls still replaces the stages: no pull (count 0), the bots follow the player
    if (preset.pulls.empty())
        return true;

    RunState const& run = Run(player, map, preset);
    DungeonPlanPull const& source = preset.pulls[run.active];
    pull.id = source.id;
    pull.index = uint32(run.active) + 1;
    pull.count = uint32(preset.pulls.size());
    pull.name = source.name;
    pull.total = uint32(source.guids.size());
    pull.alive = 0;
    for (uint32 guid : source.guids)
        if (IsSpawnAlive(map, guid))
            ++pull.alive;
    pull.cleared = pull.alive == 0;
    pull.hasPosition = pull.cleared ? source.hasCleared : source.hasStart;
    pull.position = pull.cleared ? source.cleared : source.start;
    pull.path = pull.cleared ? std::vector<DungeonPlanPoint>() : source.path;
    return true;
}

void DungeonPlannerSubsystem::StepPull(Player* player, int32 step)
{
    {
        std::lock_guard<std::mutex> lock(m_presetLock);
        DungeonPreset const* preset = nullptr;
        RunState* run = SelectedRun(player, preset);
        if (!run)
            return;
        SetActive(*run, *preset, run->active + step);
    }
    BroadcastPlanChanged(player, player->GetMapId());
}

void DungeonPlannerSubsystem::SetPull(Player* player, uint32 index)
{
    {
        std::lock_guard<std::mutex> lock(m_presetLock);
        DungeonPreset const* preset = nullptr;
        RunState* run = SelectedRun(player, preset);
        if (!run || index == 0 || index > preset->pulls.size())
            return;
        SetActive(*run, *preset, int32(index) - 1);
    }
    BroadcastPlanChanged(player, player->GetMapId());
}

// ---------------------------------------------------------------- addon messages

bool DungeonPlannerSubsystem::HandleAddonRequest(Player* player, std::string const& msg)
{
    if (msg.rfind("BOT\tplanner ", 0) != 0 && msg.rfind("BOT\tplan ", 0) != 0)
        return false;

    if (player)
    {
        std::lock_guard<std::mutex> lock(m_requestLock);
        m_requests.emplace_back(player->GetObjectGuid(), msg.substr(4));
        m_hasRequests = true;
    }
    return true;
}

void DungeonPlannerSubsystem::UpdateMap(Map* map)
{
    if (!m_hasRequests)
        return;

    std::vector<std::pair<Player*, std::string>> requests;
    {
        std::lock_guard<std::mutex> lock(m_requestLock);
        for (auto itr = m_requests.begin(); itr != m_requests.end();)
        {
            if (Player* player = map->GetPlayer(itr->first))
            {
                requests.emplace_back(player, itr->second);
                itr = m_requests.erase(itr);
            }
            else
                ++itr;
        }
        m_hasRequests = !m_requests.empty();
    }

    for (auto const& [player, msg] : requests)
        if (player->IsInWorld() && player->GetSession())
            HandleRequest(player, msg);
}

void DungeonPlannerSubsystem::HandleRequest(Player* player, std::string const& msg)
{
    std::istringstream in(msg);
    std::string word, command;
    in >> word >> command;
    ObjectGuid const guid = player->GetObjectGuid();

    if (word == "plan")
    {
        if (command == "next")
            StepPull(player, 1);
        else if (command == "prev")
            StepPull(player, -1);
        else if (command == "set")
        {
            uint32 index = 0;
            if (in >> index)
                SetPull(player, index);
        }
        SendStatus(player);
        return;
    }

    uint32 mapId = 0, presetId = 0;
    if (command == "spawns" && (in >> mapId))
        SendSpawns(player, mapId);
    else if (command == "list" && (in >> mapId))
        SendList(player, mapId);
    else if (command == "get" && (in >> presetId))
        SendPreset(player, presetId);
    else if (command == "select" && (in >> mapId >> presetId))
    {
        SelectPreset(player, mapId, presetId);
        SendList(player, mapId);
    }
    else if (command == "delete" && (in >> presetId))
    {
        std::shared_ptr<DungeonPreset const> preset = GetPreset(presetId);
        if (!preset)
            return;
        DeletePreset(presetId);
        // its selectors' bots go back to the stages
        BroadcastPlanChanged(player, preset->mapId);
        SendList(player, preset->mapId);
    }
    else if (command == "begin")
    {
        std::lock_guard<std::mutex> lock(m_presetLock);
        Upload& upload = m_uploads[guid];
        upload = Upload();
        upload.active = true;
        in >> upload.preset.id >> upload.preset.mapId;
        std::getline(in >> std::ws, upload.preset.name);
    }
    else if (command == "guids" || command == "start" || command == "cleared" || command == "path")
    {
        std::lock_guard<std::mutex> lock(m_presetLock);
        Upload& upload = m_uploads[guid];
        size_t index = 0;
        if (!upload.active || !(in >> index) || index >= MAX_PULLS)
            return;
        if (upload.preset.pulls.size() <= index)
            upload.preset.pulls.resize(index + 1);

        DungeonPlanPull& pull = upload.preset.pulls[index];
        if (command == "guids")
        {
            std::string list;
            in >> list;
            std::vector<uint32> const guids = ParseGuids(list);
            pull.guids.insert(pull.guids.end(), guids.begin(), guids.end());
            return;
        }
        DungeonPlanPoint point;
        while (in >> point.x >> point.y >> point.z)
        {
            if (command == "start")
            {
                pull.hasStart = true;
                pull.start = point;
            }
            else if (command == "cleared")
            {
                pull.hasCleared = true;
                pull.cleared = point;
            }
            else
                pull.path.push_back(point);
        }
    }
    else if (command == "end")
    {
        DungeonPreset preset;
        {
            std::lock_guard<std::mutex> lock(m_presetLock);
            auto itr = m_uploads.find(guid);
            if (itr == m_uploads.end() || !itr->second.active)
                return;
            preset = std::move(itr->second.preset);
            m_uploads.erase(itr);
        }
        uint32 const mapId = preset.mapId;
        uint32 const id = SavePreset(std::move(preset));
        {
            std::lock_guard<std::mutex> lock(m_presetLock);
            m_lastSaved[guid] = id;
        }
        // the saved preset is the one the player plans with: its bots use it (SelectPreset tells them); the bots of
        // the others who use it follow the new version on their next check
        SelectPreset(player, mapId, id);
        SendList(player, mapId);
    }
}

void DungeonPlannerSubsystem::SendChunks(Player* player, char const* prefix, std::string const& payload)
{
    size_t const count = std::max<size_t>(1, (payload.size() + CHUNK_SIZE - 1) / CHUNK_SIZE);
    uint32 const seq = ++m_chunkSeq % 256;
    for (size_t i = 0; i < count; ++i)
    {
        std::ostringstream chunk;
        chunk << prefix << "\tS" << seq << ":" << (i + 1) << "/" << count << ":" << payload.substr(i * CHUNK_SIZE, CHUNK_SIZE);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, chunk.str().c_str(), LANG_ADDON, CHAT_TAG_NONE, player->GetObjectGuid(), player->GetName());
        player->GetSession()->SendPacket(data);
    }
}

void DungeonPlannerSubsystem::SendSpawns(Player* player, uint32 mapId)
{
    std::shared_ptr<const std::vector<DungeonPack>> packs = GetPacks(mapId);
    std::ostringstream payload;
    payload << std::fixed << std::setprecision(1) << "M^" << mapId;

    std::set<uint32> entries;
    for (DungeonPack const& pack : *packs)
        for (DungeonPackSpawn const& spawn : pack.spawns)
            entries.insert(spawn.entry);
    for (uint32 entry : entries)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        std::string const name = Clean(info && info->Name ? info->Name : "creature " + std::to_string(entry));
        payload << "~E^" << entry << "^" << name << "^" << (info ? info->MaxLevel : 0) << "^" << (info ? info->Rank : 0);
    }
    for (DungeonPack const& pack : *packs)
        for (DungeonPackSpawn const& spawn : pack.spawns)
            payload << "~C^" << spawn.dbGuid << "^" << spawn.entry << "^" << spawn.x << "^" << spawn.y << "^" << spawn.z << "^" << pack.id;

    SendChunks(player, "BOTPS", payload.str());
}

void DungeonPlannerSubsystem::SendList(Player* player, uint32 mapId)
{
    uint32 lastSaved = 0;
    {
        std::lock_guard<std::mutex> lock(m_presetLock);
        auto itr = m_lastSaved.find(player->GetObjectGuid());
        if (itr != m_lastSaved.end())
            lastSaved = itr->second;
    }

    std::ostringstream payload;
    payload << "L^" << mapId << "^" << GetSelectedPreset(player->GetObjectGuid(), mapId) << "^" << lastSaved;
    for (auto const& [id, name] : GetPresets(mapId))
    {
        std::shared_ptr<DungeonPreset const> preset = GetPreset(id);
        payload << "~P^" << id << "^" << name << "^" << (preset ? preset->pulls.size() : 0);
    }
    SendChunks(player, "BOTPP", payload.str());
}

void DungeonPlannerSubsystem::SendPreset(Player* player, uint32 presetId)
{
    std::shared_ptr<DungeonPreset const> preset = GetPreset(presetId);
    if (!preset)
        return;

    std::ostringstream payload;
    payload << "D^" << preset->id << "^" << preset->mapId << "^" << preset->name;
    for (DungeonPlanPull const& pull : preset->pulls)
        payload << "~U^" << JoinGuids(pull.guids) << "^" << PointText(pull.hasStart, pull.start) << "^"
            << PointText(pull.hasCleared, pull.cleared) << "^" << JoinPoints(pull.path, ',');
    SendChunks(player, "BOTPD", payload.str());
}

void DungeonPlannerSubsystem::SendStatus(Player* player)
{
    std::ostringstream payload;
    Map* map = player->GetMap();
    {
        std::lock_guard<std::mutex> lock(m_presetLock);
        LoadPresets();
        auto selection = map && map->IsDungeon() ? m_selections.find({ player->GetObjectGuid(), map->GetId() }) : m_selections.end();
        auto itr = selection != m_selections.end() ? m_presets.find(selection->second) : m_presets.end();
        if (itr != m_presets.end() && itr->second->pulls.empty())
            payload << "A^0^0^" << Clean(itr->second->name) << "^" << itr->second->id;
        else if (itr != m_presets.end())
        {
            DungeonPreset const& preset = *itr->second;
            RunState const& run = Run(player, map, preset);
            payload << "A^" << (run.active + 1) << "^" << preset.pulls.size() << "^" << Clean(preset.name) << "^" << preset.id;
            for (DungeonPlanPull const& pull : preset.pulls)
            {
                uint32 alive = 0;
                for (uint32 guid : pull.guids)
                    if (IsSpawnAlive(map, guid))
                        ++alive;
                payload << "~U^" << Clean(pull.name) << "^" << alive << "^" << pull.guids.size();
            }
        }
        else
            payload << "A^0^0^^0";
    }
    SendChunks(player, "BOTPL", payload.str());
}

void DungeonPlannerSubsystem::Deinitialize()
{
    {
        std::lock_guard<std::recursive_mutex> lock(m_delegateLock);
        m_delegates.clear();
    }
    std::lock_guard<std::mutex> lock(m_presetLock);
    m_presets.clear();
    m_selections.clear();
    m_runs.clear();
    m_uploads.clear();
}
