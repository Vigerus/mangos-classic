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

#ifdef ENABLE_PLAYERBOTS

#include "MotionGenerators/PathfindingWorker.h"
#include "MotionGenerators/MoveMap.h"
#include "Globals/ObjectMgr.h"
#include "Server/DBCStores.h"
#include "Log/Log.h"

#include <limits>
#include <queue>
#include <shared_mutex>

namespace
{
    constexpr uint32 WORKER_THREADS = 2;
    // cost of taking a navlink (the loading screen), in yards
    constexpr float NAVLINK_COST = 30.0f;

    // a teleport area trigger: entering it on fromMap takes the unit to `to` on toMap
    struct NavLink
    {
        uint32 areaTriggerId;
        uint32 fromMap;
        Vector3 from;
        uint32 toMap;
        Vector3 to;
    };

    // built once from the area triggers (loaded at startup)
    std::vector<NavLink> const& GetNavLinks()
    {
        static const std::vector<NavLink> links = []
        {
            std::vector<NavLink> result;
            for (auto const& itr : sObjectMgr.GetAreaTriggers())
            {
                AreaTrigger const& at = itr.second;
                AreaTriggerEntry const* entry = sAreaTriggerStore.LookupEntry(itr.first);
                if (!entry || (!at.target_mapId && at.target_X == 0.0f && at.target_Y == 0.0f))
                    continue;

                result.push_back({ itr.first, entry->mapid, Vector3(entry->x, entry->y, entry->z), at.target_mapId, Vector3(at.target_X, at.target_Y, at.target_Z) });
            }
            return result;
        }();
        return links;
    }

    // a point of the graph: the start, the destination, or a navlink's entry / exit
    struct GraphPoint
    {
        uint32 mapId;
        Vector3 position;
    };

    // the navlinks (indices into GetNavLinks()) of the cheapest way from start to dest, estimated with straight
    // lines between the points of each map; false when the maps are not connected
    bool FindRoute(NavPathRequest const& request, std::vector<size_t>& route)
    {
        std::vector<NavLink> const& links = GetNavLinks();

        // 0 start, 1 dest, 2 + 2i entry of link i, 3 + 2i its exit
        std::vector<GraphPoint> points = { { request.startMapId, request.start }, { request.destMapId, request.dest } };
        points.reserve(2 + links.size() * 2);
        for (NavLink const& link : links)
        {
            points.push_back({ link.fromMap, link.from });
            points.push_back({ link.toMap, link.to });
        }

        const size_t count = points.size();
        std::vector<float> cost(count, std::numeric_limits<float>::max());
        std::vector<size_t> previous(count, SIZE_MAX);
        std::vector<bool> done(count, false);

        using Open = std::pair<float, size_t>;
        std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
        cost[0] = 0.0f;
        open.push({ 0.0f, 0 });

        auto relax = [&](size_t from, size_t to, float edge)
        {
            if (cost[from] + edge < cost[to])
            {
                cost[to] = cost[from] + edge;
                previous[to] = from;
                open.push({ cost[to], to });
            }
        };

        while (!open.empty())
        {
            const size_t current = open.top().second;
            open.pop();
            if (done[current])
                continue;
            done[current] = true;
            if (current == 1)
                break;

            // an entry is taken: it leads to its exit
            if (current >= 2 && (current - 2) % 2 == 0)
            {
                relax(current, current + 1, NAVLINK_COST);
                continue;
            }

            // the start or an exit: walk to the destination or a navlink entry on the same map
            GraphPoint const& from = points[current];
            if (points[1].mapId == from.mapId)
                relax(current, 1, (points[1].position - from.position).length());
            for (size_t entry = 2; entry < count; entry += 2)
                if (points[entry].mapId == from.mapId && !done[entry])
                    relax(current, entry, (points[entry].position - from.position).length());
        }

        if (!done[1])
            return false;

        route.clear();
        for (size_t point = previous[1]; point != SIZE_MAX && point != 0; point = previous[point])
            if (point >= 2 && (point - 2) % 2 == 0)
                route.insert(route.begin(), (point - 2) / 2);
        return true;
    }

    // the walked leg on one map, on the calling (worker) thread's own navmesh query
    PathType BuildLeg(uint32 mapId, Vector3 const& from, Vector3 const& to, PointsArray& points)
    {
        MMAP::MMapManager* mmap = MMAP::MMapFactory::createOrGetMMapManager();
        std::shared_lock<std::shared_mutex> lock(mmap->GetLock());

        dtNavMeshQuery const* query = MMAP::MMapFactory::IsPathfindingEnabled(mapId, nullptr) ? mmap->GetThreadNavMeshQuery(mapId) : nullptr;
        PathFinder path(query, mapId);
        path.calculate(from, to);
        points = path.getPath();
        if (points.empty())
            points = { from, to };
        return path.getPathType();
    }
}

PathfindingWorker& PathfindingWorker::Instance()
{
    static PathfindingWorker instance;
    return instance;
}

std::shared_ptr<NavPathRequest> PathfindingWorker::Request(uint32 startMapId, Vector3 const& start, uint32 destMapId, Vector3 const& dest, bool useNavLinks)
{
    auto request = std::make_shared<NavPathRequest>();
    request->startMapId = startMapId;
    request->start = start;
    request->destMapId = destMapId;
    request->dest = dest;
    request->useNavLinks = useNavLinks;

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_stop)
        {
            request->state.store(NavPathState::Failed, std::memory_order_release);
            return request;
        }

        if (!m_started)
            StartThreads();
        m_queue.push_back(request);
    }
    m_wakeUp.notify_one();
    return request;
}

void PathfindingWorker::StartThreads()
{
    m_started = true;
    for (uint32 i = 0; i < WORKER_THREADS; ++i)
        m_threads.emplace_back(&PathfindingWorker::Run, this);
}

void PathfindingWorker::Shutdown()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_stop = true;
    }
    m_wakeUp.notify_all();

    for (std::thread& thread : m_threads)
        if (thread.joinable())
            thread.join();
    m_threads.clear();
}

void PathfindingWorker::Run()
{
    while (true)
    {
        std::shared_ptr<NavPathRequest> request;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wakeUp.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop)
                return;

            request = m_queue.front().lock();
            m_queue.pop_front();
        }

        // nobody waits for it any more
        if (!request)
            continue;

        Process(*request);
    }
}

void PathfindingWorker::Process(NavPathRequest& request)
{
    std::vector<size_t> route;
    if (request.useNavLinks)
    {
        if (!FindRoute(request, route))
        {
            request.state.store(NavPathState::Failed, std::memory_order_release);
            return;
        }
    }
    else if (request.startMapId != request.destMapId)
    {
        request.state.store(NavPathState::Failed, std::memory_order_release);
        return;
    }

    std::vector<NavLink> const& links = GetNavLinks();
    uint32 mapId = request.startMapId;
    Vector3 from = request.start;
    for (size_t i = 0; i <= route.size(); ++i)
    {
        NavPathSegment segment;
        segment.mapId = mapId;
        const bool last = i == route.size();
        const Vector3 to = last ? request.dest : links[route[i]].from;
        const PathType type = BuildLeg(mapId, from, to, segment.points);

        if (i == 0)
        {
            request.pathType = type;
            // the bot cannot leave where it stands
            if (type & PATHFIND_NOPATH)
            {
                request.state.store(NavPathState::Failed, std::memory_order_release);
                return;
            }
        }

        if (!last)
        {
            segment.areaTriggerId = links[route[i]].areaTriggerId;
            mapId = links[route[i]].toMap;
            from = links[route[i]].to;
        }
        request.segments.push_back(std::move(segment));
    }

    request.state.store(NavPathState::Ready, std::memory_order_release);
}

#endif
