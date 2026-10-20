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

#ifndef MANGOS_INSTANCESUBSYSTEM_H
#define MANGOS_INSTANCESUBSYSTEM_H

#include "World/WorldSubsystem.h"
#include "delegates/Delegates.h"
#include "Entities/ObjectGuid.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Map;
class Player;

// an axis aligned box of map coordinates
struct InstanceStageBox
{
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    // bots may go into the box during combat while the stage is not cleared (chase, reach, charge); out of combat
    // they never go into a stage that is not cleared
    bool bCanBeEnteredInCombatIfNotCleared = false;

    bool Contains(float x, float y, float z) const
    {
        return x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ && z <= maxZ;
    }
};

// where bots stand in a stage (the "Position Yourself" node of the dungeon trees): for the classes / specs it matches,
// the best match wins (class and spec, then class, then spec, then any)
struct InstanceStagePosition
{
    float x, y, z;
    uint32 classMask = 0;       // 1 << (class - 1) per class, 0 every class
    int32 spec = -1;            // talent tab 0..2 with the most points, -1 every spec
};

// a part of a dungeon (hall, room, boss room) made of one or more boxes. The stage at index 0 of a map is its
// entrance: always cleared. The others are cleared once no counted creature spawned inside them is alive.
struct InstanceStageDef
{
    std::string name;
    std::vector<InstanceStageBox> boxes;
    std::vector<uint32> bosses;     // creature entries of the stage's bosses; registration adds the encounter
                                    // creatures (instance_encounters kill credit) spawned in the stage
    std::vector<InstanceStagePosition> positions;   // where bots stand once the stage is cleared (optional)

    bool Contains(float x, float y, float z) const
    {
        for (InstanceStageBox const& box : boxes)
            if (box.Contains(x, y, z))
                return true;
        return false;
    }
};

// state of one stage in one instance, see InstanceSubsystem::GetStageState
struct InstanceStageState
{
    bool cleared = false;       // remembered: stays cleared when creatures respawn later
    uint32 alive = 0;           // counted creatures alive when last evaluated
    uint32 total = 0;           // counted creatures spawned in the stage
};

// the subscribed player was added to a map (world map, dungeon or battleground instance), it is in the world
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPlayerEnterMap, Player* /*player*/, Map* /*map*/);
// the subscribed player is being removed from a map (teleport, logout), still in the world during the call
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPlayerLeaveMap, Player* /*player*/, Map* /*map*/);

/*
 * Map / instance changes of single players, subscribed by the player's guid:
 *
 *   InstanceSubsystem* instances = sWorld.GetSubsystem<InstanceSubsystem>();
 *   instances->AddOnPlayerEnterMap(bot->GetObjectGuid(), this, &Foo::OnEnterMap);
 *   instances->AddOnPlayerLeaveMap(bot->GetObjectGuid(), [](Player* player, Map* map) { ... });
 *   instances->RemoveAll(bot->GetObjectGuid(), this);
 *
 * Broadcast from Map::Add(Player*) / Map::Remove(Player*) (every map type), next to InstanceData::OnPlayerEnter /
 * OnPlayerLeave. Callbacks run on the thread that moves the player while the subsystem's lock is held: keep them
 * short. Subscribing and unsubscribing from inside a callback is allowed.
 *
 * Stages: the rooms of a dungeon, so bots know which parts of an instance are cleared (boss progress alone does
 * not say). Registered once per map: LoadStages (DungeonMap constructor) reads the map's file of the
 * Instance.StagesDir directory, stages/<dungeon>.xml, whose <root map="..."> attribute names the map:
 *
 *   <root map="289" name="Scholomance">
 *     <stages>
 *       <stage name="Reliquary" box="cx cy cz ex ey ez" clearposition="x y z" canbeenteredincombatifnotcleared="false"/>
 *     </stages>
 *     <readonly> navmesh floors, creature spawns: data for the tools of contrib/stages, not read here </readonly>
 *   </root>
 *
 * box is the center and half extents of an axis aligned box; stages with the same name are one stage of several
 * boxes; the first stage is the entrance. A stage containing an encounter creature (instance_encounters) is a boss
 * stage. RegisterStages does the same from code.
 *
 * The creatures of a stage are the DB spawns (creature table) whose spawn point is inside one of its boxes, without
 * critters, uninteractible / invisible triggers and creatures friendly to players. In an instance a stage is cleared
 * when none of them is alive: a loaded creature that is dead, or one that is not loaded while its grid is (not
 * spawned: killed and removed, or never spawned by its event / spawn group), or a pending respawn time. Once
 * cleared a stage stays cleared for that instance (until the map is unloaded, then the respawn times decide again).
 * GetStageState / IsStageCleared evaluate the map, call them from the map's thread (bot AI, scripts).
 */
class InstanceSubsystem : public WorldSubsystem
{
    public:
        template <typename... Args>
        FDelegateHandle AddOnPlayerEnterMap(ObjectGuid player, Args&&... args) { return AddTo(player, &PlayerDelegates::onPlayerEnterMap, std::forward<Args>(args)...); }
        template <typename... Args>
        FDelegateHandle AddOnPlayerLeaveMap(ObjectGuid player, Args&&... args) { return AddTo(player, &PlayerDelegates::onPlayerLeaveMap, std::forward<Args>(args)...); }

        // unbinds the binding identified by handle (made by any of the AddOn* of player)
        bool Remove(ObjectGuid player, FDelegateHandle handle);
        // unbinds every member function binding of obj on player, returns the number removed
        uint32 RemoveAll(ObjectGuid player, const void* obj);

        // called by Map
        void BroadcastPlayerEnterMap(Player* player, Map* map);
        void BroadcastPlayerLeaveMap(Player* player, Map* map);

        // ---- stages

        // sets the stages of a map once (later calls for the same map are ignored), stage 0 is the entrance
        void RegisterStages(uint32 mapId, std::vector<InstanceStageDef> stages);
        // registers the stages of the map's file in Instance.StagesDir, once (nothing without a file)
        void LoadStages(uint32 mapId);
        bool HasStages(uint32 mapId) const;
        // the registered stages of a map, empty when there are none
        std::vector<InstanceStageDef> GetStages(uint32 mapId) const;
        // the first stage containing the point, -1 when none does
        int32 FindStage(uint32 mapId, float x, float y, float z) const;
        // the position of the stage that matches the class / spec best (InstanceStageDef::positions), false when none
        bool GetStagePosition(uint32 mapId, uint32 stage, uint8 classId, int32 spec, float& x, float& y, float& z) const;

        // evaluates the stage in the map (its instance), from the map's thread; stage 0 is always cleared
        InstanceStageState GetStageState(Map* map, uint32 stage);
        bool IsStageCleared(Map* map, uint32 stage) { return GetStageState(map, stage).cleared; }
        // a bot must not move to the point: it is in a box of a stage that is not cleared (other than exceptStage,
        // the stage the bot is in), out of combat or in combat when the box has !bCanBeEnteredInCombatIfNotCleared
        bool IsEntryBlocked(Map* map, float x, float y, float z, bool inCombat, int32 exceptStage = -1);
        // the db guids of the counted creatures of a stage still alive in the map (debug)
        std::vector<uint32> GetStageAliveCreatures(Map* map, uint32 stage);
        // the entries of the stage's bosses (InstanceStageDef::bosses) spawned in it and still alive in the map
        std::vector<uint32> GetStageAliveBosses(Map* map, uint32 stage);

        // called by Map when it is destroyed: drops the remembered state of the instance
        void ForgetInstance(uint32 mapId, uint32 instanceId);

        // ---- DungeonStage addon report

        // the addon request "BOT\tstages" (addon message on any chat channel, also without a group), from the chat
        // handler (world thread); answered on the player's map thread by UpdateMap. True when msg was the request.
        bool HandleStageReportRequest(Player* player, std::string const& msg);
        // called by Map::Update: answers the requests of the players on the map
        void UpdateMap(Map* map);

        virtual void Deinitialize() override;

    private:
        struct StageSpawn
        {
            uint32 dbGuid;
            uint32 entry;
            float x, y;
        };

        struct MapStages
        {
            std::vector<InstanceStageDef> stages;
            std::vector<std::vector<StageSpawn>> spawns;    // counted creatures per stage
        };

        std::shared_ptr<MapStages const> FindMapStages(uint32 mapId) const;
        // map id -> stages file of Instance.StagesDir, on the first LoadStages
        void IndexStageFiles();
        static bool ReadStagesFile(std::string const& path, std::vector<InstanceStageDef>& stages);
        // the counted spawns of the map's stages, from the creature data
        static void CollectStageSpawns(uint32 mapId, MapStages& mapStages);
        static bool IsStageSpawnAlive(Map* map, StageSpawn const& spawn, time_t now);

        static uint64 InstanceKey(uint32 mapId, uint32 instanceId) { return (uint64(mapId) << 32) | instanceId; }

        // "BOTDS\tS<seq>:<index>/<count>:<payload part>" addon messages to the player:
        //   M^<map id>^<instance id>^<stage of the player or -1>
        //   G^<stage>^<name>^<cleared 0|1>^<alive>^<total>^<alive boss name,...>
        void SendStageReport(Player* player, Map* map);

        std::mutex m_reportLock;
        std::vector<ObjectGuid> m_reportRequests;
        std::atomic<bool> m_hasReportRequests{false};
        std::atomic<uint32> m_reportSeq{0};

        std::mutex m_fileLock;
        bool m_filesIndexed = false;
        std::unordered_map<uint32, std::string> m_stageFiles;
        std::unordered_map<uint32, bool> m_loadedMaps;      // LoadStages ran for the map

        mutable std::mutex m_stageLock;
        std::unordered_map<uint32, std::shared_ptr<MapStages const>> m_mapStages;
        std::unordered_map<uint64, std::vector<bool>> m_clearedStages;  // per instance: cleared flag per stage

        struct PlayerDelegates
        {
            FOnPlayerEnterMap onPlayerEnterMap;
            FOnPlayerLeaveMap onPlayerLeaveMap;

            bool IsBound() const { return onPlayerEnterMap.IsBound() || onPlayerLeaveMap.IsBound(); }
        };

        using PlayerMap = std::unordered_map<ObjectGuid, PlayerDelegates>;

        // one argument binds a callable, two an object and its member function
        template <typename DelegateType, typename... Args>
        FDelegateHandle AddTo(ObjectGuid player, DelegateType PlayerDelegates::* member, Args&&... args)
        {
            static_assert(sizeof...(Args) == 1 || sizeof...(Args) == 2, "pass a callable, or an object and its member function");
            std::lock_guard<std::recursive_mutex> lock(m_lock);
            DelegateType& delegate = m_players[player].*member;
            m_hasPlayers = true;
            if constexpr (sizeof...(Args) == 1)
                return delegate.AddLambda(std::forward<Args>(args)...);
            else
                return delegate.Add(std::forward<Args>(args)...);
        }

        template <typename DelegateType>
        void Broadcast(Player* player, Map* map, DelegateType PlayerDelegates::* member);

        // drops the player's entry when nothing is bound to it any more (not while a broadcast runs)
        void CleanupPlayer(PlayerMap::iterator itr);

        std::recursive_mutex m_lock;
        PlayerMap m_players;
        uint32 m_broadcastDepth = 0;
        // lets broadcasts skip the lock while nobody subscribed
        std::atomic<bool> m_hasPlayers{false};
};

#endif
