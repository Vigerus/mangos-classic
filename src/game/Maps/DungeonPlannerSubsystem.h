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

#ifndef MANGOS_DUNGEONPLANNERSUBSYSTEM_H
#define MANGOS_DUNGEONPLANNERSUBSYSTEM_H

#include "World/WorldSubsystem.h"
#include "delegates/Delegates.h"
#include "Entities/ObjectGuid.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Map;
class Player;

// a creature spawn of a pack (the creature data of the map)
struct DungeonPackSpawn
{
    uint32 dbGuid;
    uint32 entry;
    float x, y, z;
};

// creatures of a dungeon that are fought together: linked or in one spawn group, or standing close to each other
struct DungeonPack
{
    uint32 id = 0;          // its lowest creature db guid
    std::string name;       // "<creature name> x<count>, ...", the boss first
    std::vector<DungeonPackSpawn> spawns;
    float x = 0.0f, y = 0.0f, z = 0.0f;     // the centre
};

struct DungeonPlanPoint
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// a pull of a preset: creatures killed together, where the bots wait for it (start), where they go once it is dead
// (cleared) and the way there from the previous pull (path, walked before the start position)
struct DungeonPlanPull
{
    uint32 id = 0;                  // its lowest creature db guid (keys the bots' rtsc spots)
    std::string name;
    std::vector<uint32> guids;      // creature db guids, sorted
    bool hasStart = false;
    DungeonPlanPoint start;
    bool hasCleared = false;
    DungeonPlanPoint cleared;
    std::vector<DungeonPlanPoint> path;
};

// a dungeon plan made in the Bot Dungeon Planner addon: the ordered pulls of a dungeon, shared by every character
struct DungeonPreset
{
    uint32 id = 0;
    uint32 mapId = 0;
    std::string name;
    std::vector<DungeonPlanPull> pulls;
};

// the active pull of a player's selected preset in its instance, as the bots read it
struct DungeonActivePull
{
    uint32 presetId = 0;
    std::string presetName;
    uint32 id = 0;                  // the pull's id
    uint32 index = 0;               // 1 based
    uint32 count = 0;
    std::string name;
    uint32 alive = 0;
    uint32 total = 0;
    bool cleared = false;           // its creatures are dead: the bots go to the cleared position
    bool hasPosition = false;       // the preset's position for the state (start / cleared)
    DungeonPlanPoint position;
    std::vector<DungeonPlanPoint> path;     // before it is cleared: the way to the start position
};

// the plan of the subscribed player changed: selector (the player itself or a member of its group) selected another
// preset for mapId or none, the selected preset was saved or deleted, or its active pull moved (Next / Prev)
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnDungeonPlanChanged, ObjectGuid /*self*/, ObjectGuid /*selector*/, uint32 /*mapId*/);

/*
 * Dungeon plans (presets) for the bots, made in the Bot Dungeon Planner client addon and kept in the characters
 * database (dungeon_planner_presets / dungeon_planner_pulls), independent of the character that made them. A player
 * selects one preset per dungeon (dungeon_planner_selections), or none: then the bots use the map's stages
 * (InstanceSubsystem). In the instance one pull of the selected preset is active, from the first one not cleared;
 * Next / Prev (DungeonStage addon) move it. The bots of the player read it on their map thread (GetActivePull).
 *
 *   DungeonPlannerSubsystem* planner = sWorld.GetSubsystem<DungeonPlannerSubsystem>();
 *   planner->AddOnPlanChanged(bot->GetObjectGuid(), this, &Foo::OnPlanChanged);
 *   DungeonActivePull pull;
 *   if (planner->GetActivePull(master, bot->GetMap(), pull)) ...
 *
 * OnPlanChanged goes to the selector and the members of its group, from the thread that changed the plan (the
 * selector's map thread): keep the callbacks short (set a flag).
 *
 * Packs: the hostile creature spawns of a dungeon grouped by spawn group, creature_linking and distance (10 yards,
 * 5 high), ordered from the entrance to the nearest pack left: the planner's creature dots, the bots' aggro checks.
 *
 * Addon messages, prefix "BOT", on any chat channel (answered on the player's map thread, as party addon messages
 * from the player itself; long answers as chunks "S<seq>:<index>/<count>:<payload part>", records '~', fields '^'):
 *   planner spawns <map>        -> BOTPS  M^<map>~E^<entry>^<name>^<level>^<rank>~...~C^<guid>^<entry>^<x>^<y>^<z>^<pack>~...
 *   planner list <map>          -> BOTPP  L^<map>^<selected preset or 0>^<last saved preset or 0>~P^<id>^<name>^<pulls>~...
 *   planner get <preset>        -> BOTPD  D^<id>^<map>^<name>~U^<guid,...>^<x,y,z or empty>^<cleared x,y,z or empty>^<x,y,z;...>~...
 *   planner begin <preset or 0> <map> <name> / guids <pull> <guid,...> / start|cleared <pull> <x> <y> <z> /
 *     path <pull> <x> <y> <z> [...] / end        saves the preset (0: a new one) and selects it for the player in its
 *                               dungeon, answers with the list
 *   planner delete <preset>     deletes it (and the bots' spots for it), answers with the list
 *   planner select <map> <preset or 0>          the player's preset for the dungeon, answers with the list
 *   plan status / next / prev   -> BOTPL  A^<active pull or 0>^<pulls>^<preset name>^<preset>~U^<pull name>^<alive>^<total>~...
 */
class DungeonPlannerSubsystem : public WorldSubsystem
{
    public:
        template <typename... Args>
        FDelegateHandle AddOnPlanChanged(ObjectGuid self, Args&&... args)
        {
            static_assert(sizeof...(Args) == 1 || sizeof...(Args) == 2, "pass a callable, or an object and its member function");
            std::lock_guard<std::recursive_mutex> lock(m_delegateLock);
            FOnDungeonPlanChanged& delegate = m_delegates[self];
            if constexpr (sizeof...(Args) == 1)
                return delegate.AddLambda(std::forward<Args>(args)...);
            else
                return delegate.Add(std::forward<Args>(args)...);
        }
        // unbinds the binding identified by handle / every member function binding of obj on self
        bool Remove(ObjectGuid self, FDelegateHandle handle);
        uint32 RemoveAll(ObjectGuid self, const void* obj);

        // ---- packs

        std::shared_ptr<const std::vector<DungeonPack>> GetPacks(uint32 mapId);
        // "<name> x<count>, ..." of the creature entries, the highest rank first, then the most frequent
        static std::string MakeName(std::vector<uint32> const& entries);
        // the spawn is alive in the map: a loaded creature that lives, or one not loaded while its grid is not either
        // and that has no respawn time pending (the map's thread)
        static bool IsSpawnAlive(Map* map, uint32 dbGuid);

        // ---- presets

        // the presets of the dungeon: id and name, by name
        std::vector<std::pair<uint32, std::string>> GetPresets(uint32 mapId);
        std::shared_ptr<DungeonPreset const> GetPreset(uint32 presetId);
        // stores the preset (id 0: a new one), returns its id
        uint32 SavePreset(DungeonPreset preset);
        // removes it, its selections and the bots' spots for it
        void DeletePreset(uint32 presetId);

        // the player's preset for the dungeon, 0 none
        uint32 GetSelectedPreset(ObjectGuid player, uint32 mapId);
        // from the player's map thread
        void SelectPreset(Player* player, uint32 mapId, uint32 presetId);

        // the active pull of the player's selected preset in map (the player's instance of it), from map's thread;
        // false without a selected preset for the map
        bool GetActivePull(Player* player, Map* map, DungeonActivePull& pull);
        // the next / previous pull (step 1 / -1), from the player's map thread
        void StepPull(Player* player, int32 step);

        // ---- addon messages

        // a "BOT\tplanner ..." / "BOT\tplan ..." addon message, from the chat handler; true when it was one
        bool HandleAddonRequest(Player* player, std::string const& msg);
        // called by Map::Update: answers the requests of the players on the map
        void UpdateMap(Map* map);

        virtual void Deinitialize() override;

    private:
        struct RunState
        {
            uint32 mapId = 0;
            uint32 instanceId = 0;
            uint32 presetId = 0;
            int32 active = -1;
        };

        struct Upload
        {
            bool active = false;
            DungeonPreset preset;
        };

        void LoadPresets();
        void HandleRequest(Player* player, std::string const& msg);
        // the run state of the player in map, (re)started at the first pull not cleared
        RunState& Run(Player* player, Map* map, DungeonPreset const& preset);
        void BroadcastPlanChanged(Player* selector, uint32 mapId);
        void BroadcastPlanChanged(ObjectGuid self, ObjectGuid selector, uint32 mapId);

        void SendChunks(Player* player, char const* prefix, std::string const& payload);
        void SendSpawns(Player* player, uint32 mapId);
        void SendList(Player* player, uint32 mapId);
        void SendPreset(Player* player, uint32 presetId);
        void SendStatus(Player* player);

        static std::vector<DungeonPack> GeneratePacks(uint32 mapId);
        static void CreateTables();

        std::mutex m_packsLock;
        std::unordered_map<uint32, std::shared_ptr<const std::vector<DungeonPack>>> m_packs;

        std::mutex m_presetLock;        // presets, selections, run states
        bool m_loaded = false;
        std::map<uint32, std::shared_ptr<DungeonPreset const>> m_presets;
        std::map<std::pair<ObjectGuid, uint32>, uint32> m_selections;  // (player, map) -> preset
        std::unordered_map<ObjectGuid, RunState> m_runs;
        std::unordered_map<ObjectGuid, Upload> m_uploads;
        std::unordered_map<ObjectGuid, uint32> m_lastSaved;

        std::mutex m_requestLock;
        std::vector<std::pair<ObjectGuid, std::string>> m_requests;
        std::atomic<bool> m_hasRequests{false};
        std::atomic<uint32> m_chunkSeq{0};

        std::recursive_mutex m_delegateLock;
        std::unordered_map<ObjectGuid, FOnDungeonPlanChanged> m_delegates;
};

#endif
