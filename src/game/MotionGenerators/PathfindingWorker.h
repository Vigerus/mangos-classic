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

#ifndef MANGOS_PATHFINDING_WORKER_H
#define MANGOS_PATHFINDING_WORKER_H

#ifdef ENABLE_PLAYERBOTS

#include "Common.h"
#include "MotionGenerators/PathFinder.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// one walked leg of a path, on one map; legs are joined by navlinks (teleport area triggers: dungeon portals)
struct NavPathSegment
{
    uint32 mapId = 0;
    PointsArray points;             // start of the leg first; may be a straight line where the map has no navmesh loaded
    uint32 areaTriggerId = 0;       // the navlink the leg ends in (entering it teleports), 0 for the last leg
};

enum class NavPathState : uint8
{
    Pending,
    Ready,
    Failed,     // no route between the maps, or no way on the navmesh
};

// A path calculated by the PathfindingWorker: read it on the requesting thread once IsDone().
struct NavPathRequest
{
    uint32 startMapId = 0;
    Vector3 start;
    uint32 destMapId = 0;
    Vector3 dest;
    bool useNavLinks = true;

    std::atomic<NavPathState> state{ NavPathState::Pending };
    std::vector<NavPathSegment> segments;   // set before state turns Ready
    PathType pathType = PATHFIND_BLANK;     // of the first leg

    bool IsDone() const { return state.load(std::memory_order_acquire) != NavPathState::Pending; }
    bool IsReady() const { return state.load(std::memory_order_acquire) == NavPathState::Ready; }
};

// Calculates paths on its own threads, so map updates do not wait for detour. The maps form one graph: the navmesh
// of each map, joined by the teleport area triggers (dungeon portals) as navlinks from the trigger to its target,
// so a path to another map walks to the portal(s) on the way. Requests are polled (NavPathRequest::IsDone); one
// that nobody holds any more is skipped.
class PathfindingWorker
{
    public:
        static PathfindingWorker& Instance();

        // a path to dest; with useNavLinks false only the direct leg on the start map (destMapId must be startMapId)
        std::shared_ptr<NavPathRequest> Request(uint32 startMapId, Vector3 const& start, uint32 destMapId, Vector3 const& dest, bool useNavLinks = true);

        // stops the threads, before the mmaps are freed
        void Shutdown();

    private:
        PathfindingWorker() = default;
        ~PathfindingWorker() { Shutdown(); }

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

#define sPathfindingWorker PathfindingWorker::Instance()

#endif

#endif
