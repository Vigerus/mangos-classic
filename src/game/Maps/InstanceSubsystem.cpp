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

#include "Maps/InstanceSubsystem.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Server/DBCStores.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Chat/Chat.h"

#include "Config/Config.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

bool InstanceSubsystem::Remove(ObjectGuid player, FDelegateHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(player);
    if (itr == m_players.end())
        return false;

    bool const removed = itr->second.onPlayerEnterMap.Remove(handle) || itr->second.onPlayerLeaveMap.Remove(handle);
    CleanupPlayer(itr);
    return removed;
}

uint32 InstanceSubsystem::RemoveAll(ObjectGuid player, const void* obj)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(player);
    if (itr == m_players.end())
        return 0;

    uint32 const removed = itr->second.onPlayerEnterMap.RemoveAll(obj) + itr->second.onPlayerLeaveMap.RemoveAll(obj);
    CleanupPlayer(itr);
    return removed;
}

template <typename DelegateType>
void InstanceSubsystem::Broadcast(Player* player, Map* map, DelegateType PlayerDelegates::* member)
{
    if (!m_hasPlayers || !player)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    auto itr = m_players.find(player->GetObjectGuid());
    if (itr == m_players.end())
        return;

    // entries are not erased while a broadcast runs, so itr stays valid even if callbacks (un)subscribe
    ++m_broadcastDepth;
    (itr->second.*member).Broadcast(player, map);
    --m_broadcastDepth;
    CleanupPlayer(itr);
}

void InstanceSubsystem::BroadcastPlayerEnterMap(Player* player, Map* map)
{
    Broadcast(player, map, &PlayerDelegates::onPlayerEnterMap);
}

void InstanceSubsystem::BroadcastPlayerLeaveMap(Player* player, Map* map)
{
    Broadcast(player, map, &PlayerDelegates::onPlayerLeaveMap);
}

void InstanceSubsystem::Deinitialize()
{
    {
        std::lock_guard<std::recursive_mutex> lock(m_lock);
        m_players.clear();
        m_hasPlayers = false;
    }

    std::lock_guard<std::mutex> lock(m_stageLock);
    m_mapStages.clear();
    m_clearedStages.clear();
}

// ---------------------------------------------------------------- stages

void InstanceSubsystem::RegisterStages(uint32 mapId, std::vector<InstanceStageDef> stages)
{
    if (stages.empty() || HasStages(mapId))
        return;

    // collected outside the lock: the creature data is read only at runtime
    auto mapStages = std::make_shared<MapStages>();
    mapStages->stages = std::move(stages);
    CollectStageSpawns(mapId, *mapStages);

    std::lock_guard<std::mutex> lock(m_stageLock);
    if (m_mapStages.emplace(mapId, mapStages).second)
    {
        for (size_t i = 0; i < mapStages->stages.size(); ++i)
            sLog.outDetail("InstanceSubsystem: map %u stage %u '%s': %u creatures", mapId, uint32(i), mapStages->stages[i].name.c_str(), uint32(mapStages->spawns[i].size()));
    }
}

void InstanceSubsystem::LoadStages(uint32 mapId)
{
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_fileLock);
        if (m_loadedMaps[mapId])
            return;
        m_loadedMaps[mapId] = true;

        IndexStageFiles();
        auto itr = m_stageFiles.find(mapId);
        if (itr == m_stageFiles.end())
            return;
        path = itr->second;
    }

    std::vector<InstanceStageDef> stages;
    if (!ReadStagesFile(path, stages))
        return;

    sLog.outString("InstanceSubsystem: %u stages for map %u from %s", uint32(stages.size()), mapId, path.c_str());
    RegisterStages(mapId, std::move(stages));
}

void InstanceSubsystem::IndexStageFiles()
{
    if (m_filesIndexed)
        return;
    m_filesIndexed = true;

    std::string const directory = sConfig.GetStringDefault("Instance.StagesDir", "stages");
    std::error_code error;
    if (directory.empty())
        return;
    if (!std::filesystem::is_directory(directory, error))
    {
        sLog.outError("InstanceSubsystem: Instance.StagesDir '%s' is not a directory, no dungeon stages", directory.c_str());
        return;
    }

    // the map from the root element at the top of the file; the rest (big readonly data) only when it is loaded
    std::regex const mapPattern(R"re(<root[^>]*\smap="(\d+)")re");
    for (auto const& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".xml")
            continue;

        std::ifstream file(entry.path());
        std::string head(512, '\0');
        file.read(&head[0], head.size());
        head.resize(size_t(file.gcount()));

        std::smatch match;
        if (!std::regex_search(head, match, mapPattern))
        {
            sLog.outError("InstanceSubsystem: %s has no <root map=\"...\">, skipped", entry.path().string().c_str());
            continue;
        }

        uint32 const mapId = uint32(std::stoul(match[1].str()));
        if (!m_stageFiles.emplace(mapId, entry.path().string()).second)
            sLog.outError("InstanceSubsystem: %s: map %u has another stages file, skipped", entry.path().string().c_str(), mapId);
    }
}

bool InstanceSubsystem::ReadStagesFile(std::string const& path, std::vector<InstanceStageDef>& stages)
{
    namespace pt = boost::property_tree;
    try
    {
        pt::ptree tree;
        pt::read_xml(path, tree, pt::xml_parser::trim_whitespace);

        std::unordered_map<std::string, size_t> byName;
        for (auto const& node : tree.get_child("root.stages"))
        {
            if (node.first != "stage")
                continue;

            pt::ptree const& stage = node.second;
            std::string const name = stage.get<std::string>("<xmlattr>.name");

            float cx, cy, cz, ex, ey, ez;
            std::istringstream box(stage.get<std::string>("<xmlattr>.box"));
            if (!(box >> cx >> cy >> cz >> ex >> ey >> ez))
                throw std::runtime_error("stage '" + name + "': box needs \"cx cy cz ex ey ez\"");

            InstanceStageBox stageBox{ cx - ex, cy - ey, cz - ez, cx + ex, cy + ey, cz + ez };
            stageBox.bCanBeEnteredInCombatIfNotCleared = stage.get<std::string>("<xmlattr>.canbeenteredincombatifnotcleared", "false") == "true";

            // stages of the same name are one stage of several boxes
            auto itr = byName.find(name);
            if (itr == byName.end())
            {
                itr = byName.emplace(name, stages.size()).first;
                InstanceStageDef def;
                def.name = name;
                stages.push_back(def);
            }
            InstanceStageDef& def = stages[itr->second];
            def.boxes.push_back(stageBox);

            std::string const clear = stage.get<std::string>("<xmlattr>.clearposition", "");
            if (clear.find_first_not_of(" \t") != std::string::npos && def.positions.empty())
            {
                InstanceStagePosition position;
                std::istringstream values(clear);
                if (!(values >> position.x >> position.y >> position.z))
                    throw std::runtime_error("stage '" + name + "': clearposition needs \"x y z\"");
                def.positions.push_back(position);
            }
        }
    }
    catch (std::exception const& e)
    {
        sLog.outError("InstanceSubsystem: %s: %s, no stages", path.c_str(), e.what());
        stages.clear();
        return false;
    }
    return !stages.empty();
}

bool InstanceSubsystem::HasStages(uint32 mapId) const
{
    return FindMapStages(mapId) != nullptr;
}

std::vector<InstanceStageDef> InstanceSubsystem::GetStages(uint32 mapId) const
{
    std::shared_ptr<MapStages const> mapStages = FindMapStages(mapId);
    return mapStages ? mapStages->stages : std::vector<InstanceStageDef>();
}

int32 InstanceSubsystem::FindStage(uint32 mapId, float x, float y, float z) const
{
    std::shared_ptr<MapStages const> mapStages = FindMapStages(mapId);
    if (!mapStages)
        return -1;

    for (size_t i = 0; i < mapStages->stages.size(); ++i)
        if (mapStages->stages[i].Contains(x, y, z))
            return int32(i);
    return -1;
}

bool InstanceSubsystem::GetStagePosition(uint32 mapId, uint32 stage, uint8 classId, int32 spec, float& x, float& y, float& z) const
{
    std::shared_ptr<MapStages const> mapStages = FindMapStages(mapId);
    if (!mapStages || stage >= mapStages->stages.size() || !classId)
        return false;

    uint32 const classBit = 1 << (classId - 1);
    InstanceStagePosition const* best = nullptr;
    int32 bestScore = -1;
    for (InstanceStagePosition const& position : mapStages->stages[stage].positions)
    {
        if ((position.classMask && !(position.classMask & classBit)) || (position.spec >= 0 && position.spec != spec))
            continue;

        int32 const score = (position.classMask ? 2 : 0) + (position.spec >= 0 ? 1 : 0);
        if (score > bestScore)
        {
            best = &position;
            bestScore = score;
        }
    }

    if (!best)
        return false;

    x = best->x;
    y = best->y;
    z = best->z;
    return true;
}

InstanceStageState InstanceSubsystem::GetStageState(Map* map, uint32 stage)
{
    InstanceStageState state;
    if (!map)
        return state;

    std::shared_ptr<MapStages const> mapStages = FindMapStages(map->GetId());
    if (!mapStages || stage >= mapStages->stages.size())
        return state;

    std::vector<StageSpawn> const& spawns = mapStages->spawns[stage];
    state.total = uint32(spawns.size());

    time_t const now = time(nullptr);
    for (StageSpawn const& spawn : spawns)
        if (IsStageSpawnAlive(map, spawn, now))
            ++state.alive;

    uint64 const key = InstanceKey(map->GetId(), map->GetInstanceId());
    std::lock_guard<std::mutex> lock(m_stageLock);
    std::vector<bool>& cleared = m_clearedStages[key];
    if (cleared.size() < mapStages->stages.size())
        cleared.resize(mapStages->stages.size(), false);

    if (stage == 0 || state.alive == 0)
        cleared[stage] = true;
    state.cleared = cleared[stage];
    return state;
}

bool InstanceSubsystem::IsEntryBlocked(Map* map, float x, float y, float z, bool inCombat, int32 exceptStage)
{
    if (!map)
        return false;

    std::shared_ptr<MapStages const> mapStages = FindMapStages(map->GetId());
    if (!mapStages)
        return false;

    for (size_t i = 1; i < mapStages->stages.size(); ++i)      // stage 0 is always cleared
    {
        if (int32(i) == exceptStage)
            continue;

        for (InstanceStageBox const& box : mapStages->stages[i].boxes)
        {
            if (!box.Contains(x, y, z) || (inCombat && box.bCanBeEnteredInCombatIfNotCleared))
                continue;

            if (!IsStageCleared(map, uint32(i)))
                return true;
            break;      // the other boxes of a cleared stage do not block either
        }
    }
    return false;
}

std::vector<uint32> InstanceSubsystem::GetStageAliveCreatures(Map* map, uint32 stage)
{
    std::vector<uint32> alive;
    if (!map)
        return alive;

    std::shared_ptr<MapStages const> mapStages = FindMapStages(map->GetId());
    if (!mapStages || stage >= mapStages->stages.size())
        return alive;

    time_t const now = time(nullptr);
    for (StageSpawn const& spawn : mapStages->spawns[stage])
    {
        if (IsStageSpawnAlive(map, spawn, now))
            alive.push_back(spawn.dbGuid);
    }
    return alive;
}

std::vector<uint32> InstanceSubsystem::GetStageAliveBosses(Map* map, uint32 stage)
{
    std::vector<uint32> alive;
    if (!map)
        return alive;

    std::shared_ptr<MapStages const> mapStages = FindMapStages(map->GetId());
    if (!mapStages || stage >= mapStages->stages.size())
        return alive;

    std::vector<uint32> const& bosses = mapStages->stages[stage].bosses;
    time_t const now = time(nullptr);
    for (StageSpawn const& spawn : mapStages->spawns[stage])
    {
        if (std::find(bosses.begin(), bosses.end(), spawn.entry) == bosses.end())
            continue;
        if (std::find(alive.begin(), alive.end(), spawn.entry) == alive.end() && IsStageSpawnAlive(map, spawn, now))
            alive.push_back(spawn.entry);
    }
    return alive;
}

void InstanceSubsystem::ForgetInstance(uint32 mapId, uint32 instanceId)
{
    std::lock_guard<std::mutex> lock(m_stageLock);
    m_clearedStages.erase(InstanceKey(mapId, instanceId));
}

// ---------------------------------------------------------------- DungeonStage addon report

bool InstanceSubsystem::HandleStageReportRequest(Player* player, std::string const& msg)
{
    if (msg.rfind("BOT\tstages", 0) != 0)
        return false;

    if (player)
    {
        std::lock_guard<std::mutex> lock(m_reportLock);
        if (std::find(m_reportRequests.begin(), m_reportRequests.end(), player->GetObjectGuid()) == m_reportRequests.end())
            m_reportRequests.push_back(player->GetObjectGuid());
        m_hasReportRequests = true;
    }
    return true;
}

void InstanceSubsystem::UpdateMap(Map* map)
{
    if (!m_hasReportRequests)
        return;

    std::vector<Player*> players;
    {
        std::lock_guard<std::mutex> lock(m_reportLock);
        for (auto itr = m_reportRequests.begin(); itr != m_reportRequests.end();)
        {
            if (Player* player = map->GetPlayer(*itr))
            {
                players.push_back(player);
                itr = m_reportRequests.erase(itr);
            }
            else
                ++itr;
        }
        m_hasReportRequests = !m_reportRequests.empty();
    }

    for (Player* player : players)
        SendStageReport(player, map);
}

void InstanceSubsystem::SendStageReport(Player* player, Map* map)
{
    if (!player->IsInWorld() || !player->GetSession())
        return;

    std::vector<InstanceStageDef> const stages = GetStages(map->GetId());
    int32 const current = FindStage(map->GetId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());

    std::ostringstream payload;
    payload << "M^" << map->GetId() << "^" << map->GetInstanceId() << "^" << current;
    for (uint32 i = 0; i < stages.size(); ++i)
    {
        InstanceStageState const state = GetStageState(map, i);
        payload << "~G^" << i << "^" << stages[i].name << "^" << (state.cleared ? 1 : 0) << "^" << state.alive << "^" << state.total << "^";

        bool first = true;
        for (uint32 entry : GetStageAliveBosses(map, i))
        {
            if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry))
            {
                payload << (first ? "" : ",") << info->Name;
                first = false;
            }
        }
    }

    // addon messages are limited to 255 chars: chunks, sent as party addon messages from the player itself
    size_t const chunkSize = 220;
    std::string const text = payload.str();
    size_t const count = std::max<size_t>(1, (text.size() + chunkSize - 1) / chunkSize);
    uint32 const seq = ++m_reportSeq % 256;
    for (size_t i = 0; i < count; ++i)
    {
        std::ostringstream chunk;
        chunk << "BOTDS\tS" << seq << ":" << (i + 1) << "/" << count << ":" << text.substr(i * chunkSize, chunkSize);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, chunk.str().c_str(), LANG_ADDON, CHAT_TAG_NONE, player->GetObjectGuid(), player->GetName());
        player->GetSession()->SendPacket(data);
    }
}

std::shared_ptr<InstanceSubsystem::MapStages const> InstanceSubsystem::FindMapStages(uint32 mapId) const
{
    std::lock_guard<std::mutex> lock(m_stageLock);
    auto itr = m_mapStages.find(mapId);
    return itr != m_mapStages.end() ? itr->second : nullptr;
}

void InstanceSubsystem::CollectStageSpawns(uint32 mapId, MapStages& mapStages)
{
    mapStages.spawns.assign(mapStages.stages.size(), {});

    auto worker = [&](CreatureDataPair const& pair) -> bool
    {
        CreatureData const& data = pair.second;
        if (data.mapid != mapId)
            return false;

        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data.id);
        if (!info)
            return false;

        // nothing the bots fight: critters, triggers (uninteractible, invisible), creatures friendly to players
        if (info->CreatureType == CREATURE_TYPE_CRITTER)
            return false;
        if ((info->UnitFlags & UNIT_FLAG_UNINTERACTIBLE) || (info->ExtraFlags & CREATURE_EXTRA_FLAG_INVISIBLE))
            return false;
        if (FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->Faction))
            if (faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER)
                return false;

        for (size_t i = 1; i < mapStages.stages.size(); ++i)        // stage 0 counts nothing
        {
            if (mapStages.stages[i].Contains(data.posX, data.posY, data.posZ))
            {
                mapStages.spawns[i].push_back({ pair.first, data.id, data.posX, data.posY });
                break;
            }
        }
        return false;
    };
    sObjectMgr.DoCreatureData(worker);

    // boss stages: the encounter creatures (kill credit) spawned in them
    std::vector<uint32> encounterCreatures;
    DungeonEncounterMapBounds const encounters = sObjectMgr.GetDungeonEncounterBoundsByMap(mapId);
    for (auto itr = encounters.first; itr != encounters.second; ++itr)
        if (itr->second.creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
            encounterCreatures.push_back(itr->second.creditEntry);

    for (size_t i = 0; i < mapStages.stages.size(); ++i)
    {
        std::vector<uint32>& bosses = mapStages.stages[i].bosses;
        for (StageSpawn const& spawn : mapStages.spawns[i])
            if (std::find(encounterCreatures.begin(), encounterCreatures.end(), spawn.entry) != encounterCreatures.end()
                && std::find(bosses.begin(), bosses.end(), spawn.entry) == bosses.end())
                bosses.push_back(spawn.entry);
    }
}

bool InstanceSubsystem::IsStageSpawnAlive(Map* map, StageSpawn const& spawn, time_t now)
{
    if (Creature* creature = map->GetCreature(spawn.dbGuid))     // prefers a living one
        return creature->IsAlive();

    // not in the map: killed and removed or not spawned while its grid is loaded, else waiting for the grid
    if (map->IsLoaded(spawn.x, spawn.y))
        return false;

    MapPersistentState* state = map->GetPersistentState();
    return !state || state->GetCreatureRespawnTime(spawn.dbGuid) <= now;
}

void InstanceSubsystem::CleanupPlayer(PlayerMap::iterator itr)
{
    if (m_broadcastDepth || itr->second.IsBound())
        return;

    m_players.erase(itr);
    m_hasPlayers = !m_players.empty();
}
