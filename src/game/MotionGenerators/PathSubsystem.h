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

#ifndef MANGOS_PATH_SUBSYSTEM_H
#define MANGOS_PATH_SUBSYSTEM_H

#ifdef ENABLE_PLAYERBOTS

#include "Common.h"
#include "MotionGenerators/PathFinder.h"
#include "World/WorldSubsystem.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Player;

// how a leg of a path ends
enum class NavLegEnd : uint8
{
    Arrive,         // the destination (the last leg)
    AreaTrigger,    // walking into a teleport area trigger (a dungeon portal)
    Flight,         // a taxi flight from the flight master at the leg's end
    Transport,      // a boat, zeppelin or elevator boarded at the leg's end
};

// one walked leg of a path, on one map
struct NavPathSegment
{
    uint32 mapId = 0;
    PointsArray points;             // start of the leg first; may be a straight line where the map has no navmesh loaded
    NavLegEnd end = NavLegEnd::Arrive;
    uint32 areaTriggerId = 0;       // AreaTrigger
    std::vector<uint32> taxiNodes;  // Flight: the taxi nodes flown over, the flight master's node first
    uint32 transportEntry = 0;      // Transport: its gameobject entry
    Vector3 boardPoint;             // Transport: where the transport stands when it is boarded
    uint32 leaveMapId = 0;          // Transport: where it stands when it is left
    Vector3 leavePoint;
};

enum class NavPathState : uint8
{
    Pending,
    Ready,
    Failed,     // no route between the maps, or no way on the navmesh
};

// who travels: what the route may use (flights the player knows and can pay, area trigger levels), how fast it walks
struct NavTraveler
{
    uint32 level = 0;
    bool alliance = false;
    uint32 money = 0;
    bool flights = false;
    std::vector<bool> taxiNodes;    // known taxi nodes, by node id
    float runSpeed = 7.0f;
    float swimSpeed = 4.72f;

    static NavTraveler Of(Player const* player, bool flights = true);
};

// A path calculated by the PathSubsystem: read it on the requesting thread once IsDone().
struct NavPathRequest
{
    uint32 startMapId = 0;
    Vector3 start;
    uint32 destMapId = 0;
    Vector3 dest;
    bool useGraph = true;
    NavTraveler traveler;

    std::atomic<NavPathState> state{ NavPathState::Pending };
    std::vector<NavPathSegment> segments;   // set before state turns Ready
    PathType pathType = PATHFIND_BLANK;     // of the first leg
    bool viaGraph = false;                  // the route uses the travel graph

    bool IsDone() const { return state.load(std::memory_order_acquire) != NavPathState::Pending; }
    bool IsReady() const { return state.load(std::memory_order_acquire) == NavPathState::Ready; }
};

// Calculates paths on its own threads, so map updates do not wait for detour. A short way on one map is a navmesh
// path. A long one goes over the travel graph (ai_playerbot_travelnode*: nodes at towns, flight masters, docks and
// portals joined by stored walks, flights, boats and area triggers, plus a node at both ends of every teleport area
// trigger): a navmesh path to a nearby node, the cheapest route over the graph, a navmesh path from its last node to
// the destination. The graph is loaded on the first long request. Requests are polled (NavPathRequest::IsDone); one
// that nobody holds any more is skipped.
class PathSubsystem : public WorldSubsystem
{
    public:
        ~PathSubsystem() override { Shutdown(); }

        void Deinitialize() override { Shutdown(); }

        // a path to dest; with useGraph false only the navmesh leg on the start map (destMapId must be startMapId)
        std::shared_ptr<NavPathRequest> Request(uint32 startMapId, Vector3 const& start, uint32 destMapId, Vector3 const& dest, bool useGraph = true,
            NavTraveler const& traveler = NavTraveler());

        // stops the threads, before the mmaps are freed
        void Shutdown();

        // the last point of the piece of a path walked as one spline from points[first]: a spline's points stay within
        // 256 yards of its middle (MoveSplineInitArgs::_checkPathBounds), a long path is walked in pieces
        static size_t SplinePieceEnd(PointsArray const& points, size_t first);

    private:
        void StartThreads();
        void Run();
        void Process(NavPathRequest& request);

        std::mutex m_mutex;
        std::condition_variable m_wakeUp;
        std::deque<std::weak_ptr<NavPathRequest>> m_queue;
        std::vector<std::thread> m_threads;
        bool m_stop = false;
        bool m_started = false;
};

#endif

#endif
