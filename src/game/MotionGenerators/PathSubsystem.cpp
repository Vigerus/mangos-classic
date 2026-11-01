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

#include "MotionGenerators/PathSubsystem.h"
#include "MotionGenerators/MoveMap.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Server/DBCStores.h"
#include "Log/Log.h"
#include "Util/Timer.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr uint32 WORKER_THREADS = 2;
    // on one map and closer than this: a navmesh path, without the travel graph
    constexpr float SHORT_RANGE = 200.0f;
    // the graph nodes nearest to the start and to the destination tried as the way onto / off the graph
    constexpr uint32 ENTRY_NODES = 5;
    // straight legs (onto / off the graph) found unwalkable are left out and the route searched again
    constexpr uint32 ROUTE_ATTEMPTS = 6;
    // a navmesh leg reaches its target when it ends this close to it
    constexpr float REACH_DISTANCE = 5.0f;
    // a straight leg is this much longer walked than its line
    constexpr float STRAIGHT_DETOUR = 1.3f;
    // route costs are in seconds
    constexpr float AREA_TRIGGER_COST = 10.0f;      // the loading screen
    constexpr float FLIGHT_SPEED = 32.0f;
    constexpr float FLIGHT_COST = 15.0f;            // talking to the flight master, landing
    constexpr float BOAT_COST = 150.0f;             // waiting for it and the crossing
    constexpr float ELEVATOR_COST = 20.0f;
    constexpr float ELEVATOR_WAIT_DISTANCE = 8.0f;
    // a trigger's end that the stored graph does not have is joined to this many graph nodes of its map
    constexpr uint32 TRIGGER_LINKS = 3;

    enum class EdgeKind : uint8
    {
        Straight,       // walked on a navmesh path built when the route is used
        Walk,           // a stored walk of the travel graph
        AreaTrigger,
        Transport,
        Flight,
    };

    struct GraphEdge
    {
        uint32 to = 0;
        EdgeKind kind = EdgeKind::Walk;
        uint32 object = 0;              // area trigger id, transport entry, taxi path id
        float length = 0.0f;
        float swimLength = 0.0f;
        uint8 maxCreature[3] = { 0, 0, 0 };     // the highest level along it: of mobs, of the two factions
        PointsArray points;             // Walk: the stored walk; Flight: the flight, for its length
    };

    struct GraphNode
    {
        uint32 mapId = 0;
        Vector3 position;
        std::string name;
        std::vector<GraphEdge> edges;
        int32 dock = -1;                // a transport's node: the node on the pier where it is boarded
    };

    // the travel graph, read only once loaded
    struct TravelGraph
    {
        std::vector<GraphNode> nodes;
        std::unordered_map<uint32, std::vector<uint32>> mapNodes;

        // the count nodes of the map nearest to position, nearest first
        std::vector<uint32> Nearest(uint32 mapId, Vector3 const& position, uint32 count) const
        {
            auto itr = mapNodes.find(mapId);
            if (itr == mapNodes.end())
                return {};
            std::vector<std::pair<float, uint32>> byDistance;
            for (uint32 node : itr->second)
                byDistance.push_back({ (nodes[node].position - position).squaredLength(), node });
            const size_t keep = std::min<size_t>(count, byDistance.size());
            std::partial_sort(byDistance.begin(), byDistance.begin() + keep, byDistance.end());
            std::vector<uint32> result;
            for (size_t i = 0; i < keep; ++i)
                result.push_back(byDistance[i].second);
            return result;
        }

        uint32 AddNode(uint32 mapId, Vector3 const& position, std::string name)
        {
            GraphNode node;
            node.mapId = mapId;
            node.position = position;
            node.name = std::move(name);
            nodes.push_back(std::move(node));
            mapNodes[mapId].push_back(uint32(nodes.size() - 1));
            return uint32(nodes.size() - 1);
        }

        void Load();
        void AddAreaTriggers(std::unordered_set<uint32> const& stored);
    };

    float Length(PointsArray const& points)
    {
        float length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
            length += (points[i] - points[i - 1]).length();
        return length;
    }

    void TravelGraph::Load()
    {
        const uint32 startMs = WorldTimer::getMSTime();
        std::unordered_map<uint32, uint32> byId;    // db id -> node
        if (auto result = WorldDatabase.Query("SELECT `id`, `name`, `map_id`, `x`, `y`, `z` FROM `ai_playerbot_travelnode`"))
        {
            do
            {
                Field* fields = result->Fetch();
                byId[fields[0].GetUInt32()] = AddNode(fields[2].GetUInt32(), Vector3(fields[3].GetFloat(), fields[4].GetFloat(), fields[5].GetFloat()), fields[1].GetCppString());
            }
            while (result->NextRow());
        }

        // (from node, to node) -> its edge, for the stored points
        auto key = [](uint32 from, uint32 to) { return (uint64(from) << 32) | to; };
        std::unordered_map<uint64, std::pair<uint32, size_t>> edgeOf;
        std::unordered_set<uint32> storedTriggers;
        if (auto result = WorldDatabase.Query("SELECT `node_id`, `to_node_id`, `type`, `object`, `distance`, `swim_distance`, `max_creature_0`, `max_creature_1`, `max_creature_2` FROM `ai_playerbot_travelnode_link`"))
        {
            do
            {
                Field* fields = result->Fetch();
                auto from = byId.find(fields[0].GetUInt32());
                auto to = byId.find(fields[1].GetUInt32());
                if (from == byId.end() || to == byId.end())
                    continue;

                GraphEdge edge;
                edge.to = to->second;
                // the stored link types: 1 walk, 2 area trigger, 3 transport, 4 flight (TravelNodePathType)
                switch (fields[2].GetUInt8())
                {
                    case 1: edge.kind = EdgeKind::Walk; break;
                    case 2: edge.kind = EdgeKind::AreaTrigger; break;
                    case 3: edge.kind = EdgeKind::Transport; break;
                    case 4: edge.kind = EdgeKind::Flight; break;
                    default: continue;
                }
                edge.object = fields[3].GetUInt32();
                edge.length = fields[4].GetFloat();
                edge.swimLength = fields[5].GetFloat();
                for (int i = 0; i < 3; ++i)
                    edge.maxCreature[i] = fields[6 + i].GetUInt8();
                if (edge.kind == EdgeKind::AreaTrigger)
                    storedTriggers.insert(edge.object);

                std::vector<GraphEdge>& edges = nodes[from->second].edges;
                edgeOf[key(from->second, to->second)] = { from->second, edges.size() };
                edges.push_back(std::move(edge));
            }
            while (result->NextRow());
        }

        size_t pointCount = 0;
        if (auto result = WorldDatabase.Query("SELECT `node_id`, `to_node_id`, `x`, `y`, `z` FROM `ai_playerbot_travelnode_path` ORDER BY `node_id`, `to_node_id`, `nr`"))
        {
            do
            {
                Field* fields = result->Fetch();
                auto from = byId.find(fields[0].GetUInt32());
                auto to = byId.find(fields[1].GetUInt32());
                if (from == byId.end() || to == byId.end())
                    continue;
                auto edge = edgeOf.find(key(from->second, to->second));
                if (edge == edgeOf.end())
                    continue;
                GraphEdge& stored = nodes[edge->second.first].edges[edge->second.second];
                if (stored.kind != EdgeKind::Walk && stored.kind != EdgeKind::Flight)
                    continue;
                stored.points.push_back(Vector3(fields[2].GetFloat(), fields[3].GetFloat(), fields[4].GetFloat()));
                ++pointCount;
            }
            while (result->NextRow());
        }

        for (uint32 i = 0; i < nodes.size(); ++i)
        {
            GraphNode& node = nodes[i];
            for (GraphEdge& edge : node.edges)
            {
                if (edge.kind == EdgeKind::Flight)
                    edge.length = edge.points.size() > 1 ? Length(edge.points) : (nodes[edge.to].position - node.position).length();
                // a transport is boarded from its dock, a walk edge to "<transport> dock"
                if (edge.kind == EdgeKind::Transport && node.dock < 0)
                    for (GraphEdge const& walk : node.edges)
                        if (walk.kind == EdgeKind::Walk && nodes[walk.to].name == node.name + " dock")
                            node.dock = int32(walk.to);
            }
        }

        AddAreaTriggers(storedTriggers);

        sLog.outString(">> PathSubsystem: travel graph loaded, %u nodes, %u stored path points, in %u ms", uint32(nodes.size()), uint32(pointCount),
            WorldTimer::getMSTimeDiff(startMs, WorldTimer::getMSTime()));
    }

    // every teleport area trigger the stored graph lacks (dungeon portals, their exits): a node at its entry and one at
    // its target, joined to a few graph nodes of their maps by straight legs; on a map without them (a dungeon) the
    // trigger nodes are joined to each other
    void TravelGraph::AddAreaTriggers(std::unordered_set<uint32> const& stored)
    {
        std::vector<uint32> added;
        const size_t storedNodes = nodes.size();
        for (auto const& itr : sObjectMgr.GetAreaTriggers())
        {
            AreaTrigger const& at = itr.second;
            AreaTriggerEntry const* entry = sAreaTriggerStore.LookupEntry(itr.first);
            if (!entry || (!at.target_mapId && at.target_X == 0.0f && at.target_Y == 0.0f) || stored.count(itr.first))
                continue;

            const std::string name = "Area trigger " + std::to_string(itr.first);
            const uint32 from = AddNode(entry->mapid, Vector3(entry->x, entry->y, entry->z), name);
            const uint32 to = AddNode(at.target_mapId, Vector3(at.target_X, at.target_Y, at.target_Z), name + " exit");
            GraphEdge edge;
            edge.to = to;
            edge.kind = EdgeKind::AreaTrigger;
            edge.object = itr.first;
            nodes[from].edges.push_back(edge);
            added.push_back(from);
            added.push_back(to);
        }

        auto link = [this](uint32 a, uint32 b)
        {
            const float length = (nodes[a].position - nodes[b].position).length();
            GraphEdge edge;
            edge.kind = EdgeKind::Straight;
            edge.length = length;
            edge.to = b;
            nodes[a].edges.push_back(edge);
            edge.to = a;
            nodes[b].edges.push_back(edge);
        };

        for (uint32 node : added)
        {
            bool hasStored = false;
            for (uint32 other : Nearest(nodes[node].mapId, nodes[node].position, TRIGGER_LINKS * 4))
                if (other < storedNodes)
                {
                    hasStored = true;
                    break;
                }
            if (hasStored)
            {
                uint32 linked = 0;
                for (uint32 other : Nearest(nodes[node].mapId, nodes[node].position, TRIGGER_LINKS * 4))
                    if (other < storedNodes && linked++ < TRIGGER_LINKS)
                        link(node, other);
            }
            else
            {
                // later trigger nodes of the map link back to this one
                for (uint32 other : mapNodes[nodes[node].mapId])
                    if (other < node)
                        link(node, other);
            }
        }
    }

    TravelGraph const& GetGraph()
    {
        static TravelGraph graph;
        static std::once_flag loaded;
        std::call_once(loaded, [] { graph.Load(); });
        return graph;
    }

    // the worker thread's queries for long searches, per map: a node pool big enough to search around a city
    struct LongQueries
    {
        struct Entry
        {
            dtNavMesh const* navMesh = nullptr;
            dtNavMeshQuery* query = nullptr;
        };
        std::unordered_map<uint32, Entry> maps;

        ~LongQueries()
        {
            for (auto& [mapId, entry] : maps)
                dtFreeNavMeshQuery(entry.query);
        }

        // hold the MMapManager's lock
        dtNavMeshQuery const* Get(MMAP::MMapManager* mmap, uint32 mapId)
        {
            static constexpr int maxNodes = 65535;
            dtNavMesh const* navMesh = mmap->GetNavMesh(mapId);
            if (!navMesh)
                return nullptr;

            Entry& entry = maps[mapId];
            if (entry.query && entry.navMesh == navMesh)
                return entry.query;

            // new, or the map's navmesh was loaded again
            if (!entry.query)
                entry.query = dtAllocNavMeshQuery();
            entry.navMesh = navMesh;
            if (!entry.query || dtStatusFailed(entry.query->init(navMesh, maxNodes)))
            {
                dtFreeNavMeshQuery(entry.query);
                maps.erase(mapId);
                return nullptr;
            }
            return entry.query;
        }
    };

    // a long way around (a city wall, a mountain): detour with many more polygons and nodes than the PathFinder,
    // whose limits are made for short chases and end a long way at the point nearest the destination, again and
    // again; false when the destination cannot be reached either
    bool BuildLongLeg(dtNavMeshQuery const* query, Vector3 const& from, Vector3 const& to, PointsArray& points)
    {
        static constexpr int maxPolys = 2048;

        // bot navigation, as PathFinder::createFilter without a unit
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA_SLIME | NAV_GROUND_STEEP);
        filter.setAreaCost(9, 20.0f);   // water
        filter.setAreaCost(12, 5.0f);   // mob proximity
        filter.setAreaCost(13, 20.0f);  // mob aggro

        // detour is y, z, x
        const float start[VERTEX_SIZE] = { from.y, from.z, from.x };
        const float end[VERTEX_SIZE] = { to.y, to.z, to.x };
        dtPolyRef startRef = INVALID_POLYREF, endRef = INVALID_POLYREF;
        float startPoint[VERTEX_SIZE], endPoint[VERTEX_SIZE];
        if (dtStatusFailed(query->findNearestPoly(start, FarPolySearchBound, &filter, &startRef, startPoint)) || startRef == INVALID_POLYREF ||
            dtStatusFailed(query->findNearestPoly(end, FarPolySearchBound, &filter, &endRef, endPoint)) || endRef == INVALID_POLYREF)
            return false;

        std::vector<dtPolyRef> polys(maxPolys);
        int polyCount = 0;
        const dtStatus found = query->findPath(startRef, endRef, startPoint, endPoint, &filter, polys.data(), &polyCount, maxPolys);

        if (dtStatusFailed(found) || dtStatusDetail(found, DT_PARTIAL_RESULT) || !polyCount || polys[polyCount - 1] != endRef)
            return false;

        std::vector<float> corners(maxPolys * VERTEX_SIZE);
        int cornerCount = 0;
        if (dtStatusFailed(query->findStraightPath(startPoint, endPoint, polys.data(), polyCount, corners.data(), nullptr, nullptr, &cornerCount, maxPolys)) ||
            cornerCount < 2)
            return false;

        points.clear();
        for (int i = 0; i < cornerCount; ++i)
            points.push_back(Vector3(corners[i * VERTEX_SIZE + 2], corners[i * VERTEX_SIZE], corners[i * VERTEX_SIZE + 1]));
        return true;
    }

    // the navmesh leg on one map, on the calling (worker) thread's own navmesh query; the mover walks it in pieces
    PathType BuildLeg(uint32 mapId, Vector3 const& from, Vector3 const& to, PointsArray& points)
    {
        thread_local LongQueries longQueries;

        MMAP::MMapManager* mmap = MMAP::MMapFactory::createOrGetMMapManager();
        std::shared_lock<std::shared_mutex> lock(mmap->GetLock());

        dtNavMeshQuery const* query = MMAP::MMapFactory::IsPathfindingEnabled(mapId, nullptr) ? mmap->GetThreadNavMeshQuery(mapId) : nullptr;
        PathFinder path(query, mapId);
        path.calculate(from, to);
        points = path.getPath();
        const PathType type = path.getPathType();
        // cut short: the long search, here on the worker thread where it holds up no map
        if (query && (type & (PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_SHORT)))
            if (dtNavMeshQuery const* longQuery = longQueries.Get(mmap, mapId))
                if (BuildLongLeg(longQuery, from, to, points))
                    return PATHFIND_NORMAL;
        if (points.empty())
            points = { from, to };
        return type;
    }

    // the leg gets there (not a navmesh path ending at the point nearest to it)
    bool Reaches(PathType type, PointsArray const& points, Vector3 const& to)
    {
        return !(type & PATHFIND_NOPATH) && !points.empty() && (points.back() - to).length() <= REACH_DISTANCE;
    }

    // the route over the graph: start and destination are the extra nodes after the graph's
    class RouteSearch
    {
        public:
            RouteSearch(TravelGraph const& graph, NavPathRequest const& request)
                : m_graph(graph), m_request(request), m_start(uint32(graph.nodes.size())), m_dest(m_start + 1)
            {
                m_startNodes = graph.Nearest(request.startMapId, request.start, ENTRY_NODES);
                m_destNodes = graph.Nearest(request.destMapId, request.dest, ENTRY_NODES);
            }

            // the navmesh path straight to the destination is known not to get there
            void ForbidDirect() { m_forbidden.insert(Key(m_start, m_dest)); }

            // false when the maps are not connected for the traveler
            bool Build(std::vector<NavPathSegment>& segments)
            {
                for (uint32 attempt = 0; attempt < ROUTE_ATTEMPTS; ++attempt)
                {
                    std::vector<Hop> hops;
                    if (!Search(hops))
                        return false;
                    if (Assemble(hops, segments))
                        return true;
                }
                return false;
            }

        private:
            struct Hop
            {
                uint32 from, to;
                GraphEdge const* edge;  // nullptr for a straight leg onto / off the graph
            };

            uint32 MapOf(uint32 node) const { return node == m_start ? m_request.startMapId : node == m_dest ? m_request.destMapId : m_graph.nodes[node].mapId; }
            Vector3 const& PositionOf(uint32 node) const { return node == m_start ? m_request.start : node == m_dest ? m_request.dest : m_graph.nodes[node].position; }
            static uint64 Key(uint32 from, uint32 to) { return (uint64(from) << 32) | to; }

            float StraightCost(uint32 from, uint32 to) const
            {
                return (PositionOf(to) - PositionOf(from)).length() * STRAIGHT_DETOUR / std::max(1.0f, m_request.traveler.runSpeed);
            }

            // seconds, negative when the traveler cannot take it
            float Cost(uint32 from, GraphEdge const& edge) const
            {
                NavTraveler const& traveler = m_request.traveler;
                switch (edge.kind)
                {
                    case EdgeKind::Straight:
                        return StraightCost(from, edge.to);
                    case EdgeKind::Walk:
                    {
                        float modifier = 1.0f;
                        if (traveler.level)
                        {
                            // mobs 10 levels above the traveler and more cost 10% per level, the other faction 30%
                            const int32 mobs = int32(edge.maxCreature[0]) - int32(traveler.level) - 10;
                            const int32 faction = int32(edge.maxCreature[traveler.alliance ? 2 : 1]) - int32(traveler.level) - 10;
                            if (mobs > 0)
                                modifier += 0.1f * mobs;
                            if (faction > 0)
                                modifier += 0.3f * faction;
                        }
                        const float swim = std::min(edge.swimLength, edge.length);
                        return ((edge.length - swim) / std::max(1.0f, traveler.runSpeed) + swim / std::max(1.0f, traveler.swimSpeed)) * modifier;
                    }
                    case EdgeKind::AreaTrigger:
                    {
                        AreaTrigger const* at = sObjectMgr.GetAreaTrigger(edge.object);
                        if (at && at->requiredLevel > traveler.level && traveler.level)
                            return -1.0f;
                        return AREA_TRIGGER_COST;
                    }
                    case EdgeKind::Transport:
                    {
                        GraphNode const& to = m_graph.nodes[edge.to];
                        GraphNode const& fromNode = m_graph.nodes[from];
                        const bool boat = to.mapId != fromNode.mapId || (to.position - fromNode.position).length() > 100.0f;
                        return boat ? BOAT_COST : ELEVATOR_COST;
                    }
                    case EdgeKind::Flight:
                    {
                        if (!traveler.flights)
                            return -1.0f;
                        TaxiPathEntry const* taxiPath = sTaxiPathStore.LookupEntry(edge.object);
                        if (!taxiPath || taxiPath->price > traveler.money)
                            return -1.0f;
                        // the destination must be known; the start is learned from its flight master
                        if (taxiPath->to >= traveler.taxiNodes.size() || !traveler.taxiNodes[taxiPath->to])
                            return -1.0f;
                        TaxiNodesEntry const* fromNode = sTaxiNodesStore.LookupEntry(taxiPath->from);
                        TaxiNodesEntry const* toNode = sTaxiNodesStore.LookupEntry(taxiPath->to);
                        const int side = traveler.alliance ? 1 : 0;
                        if (!fromNode || !toNode || !fromNode->MountCreatureID[side] || !toNode->MountCreatureID[side])
                            return -1.0f;
                        return edge.length / FLIGHT_SPEED + FLIGHT_COST;
                    }
                }
                return -1.0f;
            }

            // Dijkstra over the graph with the start and the destination joined to their nearest nodes
            bool Search(std::vector<Hop>& hops) const
            {
                const uint32 count = m_dest + 1;
                std::vector<float> cost(count, std::numeric_limits<float>::max());
                std::vector<uint32> previous(count, UINT32_MAX);
                std::vector<GraphEdge const*> previousEdge(count, nullptr);
                std::vector<bool> done(count, false);
                std::vector<bool> leadsOff(count, false);
                for (uint32 node : m_destNodes)
                    leadsOff[node] = true;

                using Open = std::pair<float, uint32>;
                std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
                auto relax = [&](uint32 from, uint32 to, float edgeCost, GraphEdge const* edge)
                {
                    if (edgeCost < 0.0f || done[to] || m_forbidden.count(Key(from, to)))
                        return;
                    if (cost[from] + edgeCost < cost[to])
                    {
                        cost[to] = cost[from] + edgeCost;
                        previous[to] = from;
                        previousEdge[to] = edge;
                        open.push({ cost[to], to });
                    }
                };

                cost[m_start] = 0.0f;
                open.push({ 0.0f, m_start });
                while (!open.empty())
                {
                    const uint32 current = open.top().second;
                    open.pop();
                    if (done[current])
                        continue;
                    done[current] = true;
                    if (current == m_dest)
                        break;

                    if (current == m_start)
                    {
                        for (uint32 node : m_startNodes)
                            relax(current, node, StraightCost(current, node), nullptr);
                        if (m_request.startMapId == m_request.destMapId)
                            relax(current, m_dest, StraightCost(current, m_dest), nullptr);
                        continue;
                    }

                    for (GraphEdge const& edge : m_graph.nodes[current].edges)
                        relax(current, edge.to, Cost(current, edge), &edge);
                    if (leadsOff[current])
                        relax(current, m_dest, StraightCost(current, m_dest), nullptr);
                }

                if (!done[m_dest])
                    return false;

                hops.clear();
                for (uint32 node = m_dest; node != m_start; node = previous[node])
                    hops.insert(hops.begin(), Hop{ previous[node], node, previousEdge[node] });
                return true;
            }

            // the navmesh path of a straight leg; false (and the leg left out of the next search) when it does not get
            // there on the start map, a straight line on another (its navmesh may not be loaded: planned again there)
            bool StraightLeg(uint32 from, uint32 to, PointsArray& points)
            {
                const uint32 mapId = MapOf(from);
                const PathType type = BuildLeg(mapId, PositionOf(from), PositionOf(to), points);
                if (Reaches(type, points, PositionOf(to)))
                    return true;
                if (mapId != m_request.startMapId)
                {
                    points = { PositionOf(from), PositionOf(to) };
                    return true;
                }
                m_forbidden.insert(Key(from, to));
                return false;
            }

            static void Append(NavPathSegment& segment, PointsArray const& points, size_t first = 0)
            {
                for (size_t i = first; i < points.size(); ++i)
                    if (segment.points.empty() || (points[i] - segment.points.back()).length() > 0.5f)
                        segment.points.push_back(points[i]);
            }

            static size_t Closest(PointsArray const& points, Vector3 const& position)
            {
                size_t closest = 0;
                for (size_t i = 1; i < points.size(); ++i)
                    if ((points[i] - position).squaredLength() < (points[closest] - position).squaredLength())
                        closest = i;
                return closest;
            }

            // the points end distance short of target, on the way there
            static void StopShort(PointsArray& points, Vector3 const& target, float distance)
            {
                while (points.size() > 1 && (points.back() - target).length() < distance)
                    points.pop_back();
                Vector3 const& last = points.back();
                const float away = (target - last).length();
                if (away > distance)
                    points.push_back(target + (last - target) * (distance / away));
            }

            bool Assemble(std::vector<Hop> const& hops, std::vector<NavPathSegment>& segments)
            {
                segments.clear();
                NavPathSegment current;
                current.mapId = m_request.startMapId;
                current.points.push_back(m_request.start);
                auto next = [&](uint32 node, Vector3 const& from)
                {
                    segments.push_back(std::move(current));
                    current = NavPathSegment();
                    current.mapId = MapOf(node);
                    current.points.push_back(from);
                };

                for (Hop const& hop : hops)
                {
                    if (!hop.edge || hop.edge->kind == EdgeKind::Straight)
                    {
                        PointsArray points;
                        if (!StraightLeg(hop.from, hop.to, points))
                            return false;
                        Append(current, points);
                        continue;
                    }

                    GraphEdge const& edge = *hop.edge;
                    GraphNode const& fromNode = m_graph.nodes[hop.from];
                    GraphNode const& toNode = m_graph.nodes[hop.to];
                    switch (edge.kind)
                    {
                        case EdgeKind::Walk:
                            // off a transport: from its dock, not from where the transport stood
                            Append(current, edge.points, fromNode.dock >= 0 && current.points.size() == 1 ? Closest(edge.points, current.points[0]) : 0);
                            if (edge.points.empty())
                                Append(current, { toNode.position });
                            break;
                        case EdgeKind::AreaTrigger:
                            current.end = NavLegEnd::AreaTrigger;
                            current.areaTriggerId = edge.object;
                            next(hop.to, toNode.position);
                            break;
                        case EdgeKind::Flight:
                        {
                            TaxiPathEntry const* taxiPath = sTaxiPathStore.LookupEntry(edge.object);
                            if (!taxiPath)
                                return false;
                            // flown on: one flight over several taxi nodes
                            if (!segments.empty() && segments.back().end == NavLegEnd::Flight && current.points.size() == 1 &&
                                segments.back().taxiNodes.back() == taxiPath->from)
                            {
                                segments.back().taxiNodes.push_back(taxiPath->to);
                                current.mapId = toNode.mapId;
                                current.points[0] = toNode.position;
                                break;
                            }
                            current.end = NavLegEnd::Flight;
                            current.taxiNodes = { taxiPath->from, taxiPath->to };
                            next(hop.to, toNode.position);
                            break;
                        }
                        case EdgeKind::Transport:
                        {
                            // waited for on its dock: the walk there ends at the dock, not in the water where it stands
                            if (fromNode.dock >= 0)
                            {
                                const Vector3& dock = m_graph.nodes[fromNode.dock].position;
                                current.points.resize(Closest(current.points, dock) + 1);
                                Append(current, { dock });
                            }
                            // an elevator: waited for beside its shaft, the platform may be at the other end
                            else
                                StopShort(current.points, fromNode.position, ELEVATOR_WAIT_DISTANCE);
                            current.end = NavLegEnd::Transport;
                            current.transportEntry = edge.object;
                            current.boardPoint = fromNode.position;
                            current.leaveMapId = toNode.mapId;
                            current.leavePoint = toNode.position;
                            next(hop.to, toNode.dock >= 0 ? m_graph.nodes[toNode.dock].position : toNode.position);
                            break;
                        }
                        default:
                            break;
                    }
                }
                segments.push_back(std::move(current));
                return true;
            }

            TravelGraph const& m_graph;
            NavPathRequest const& m_request;
            const uint32 m_start;
            const uint32 m_dest;
            std::vector<uint32> m_startNodes;
            std::vector<uint32> m_destNodes;
            std::unordered_set<uint64> m_forbidden;    // straight legs found unwalkable
    };
}

NavTraveler NavTraveler::Of(Player const* player, bool flights)
{
    NavTraveler traveler;
    traveler.level = player->GetLevel();
    traveler.alliance = player->GetTeam() == ALLIANCE;
    traveler.money = player->GetMoney();
    traveler.flights = flights;
    const uint32 nodeCount = std::min<uint32>(sTaxiNodesStore.GetNumRows(), TaxiMaskSize * 32 + 1);
    traveler.taxiNodes.assign(nodeCount, false);
    for (uint32 node = 1; node < nodeCount; ++node)
        traveler.taxiNodes[node] = player->isTaxiCheater() || player->m_taxi.IsTaximaskNodeKnown(node);
    traveler.runSpeed = player->GetSpeed(MOVE_RUN);
    traveler.swimSpeed = player->GetSpeed(MOVE_SWIM);
    return traveler;
}

std::shared_ptr<NavPathRequest> PathSubsystem::Request(uint32 startMapId, Vector3 const& start, uint32 destMapId, Vector3 const& dest, bool useGraph,
    NavTraveler const& traveler)
{
    auto request = std::make_shared<NavPathRequest>();
    request->startMapId = startMapId;
    request->start = start;
    request->destMapId = destMapId;
    request->dest = dest;
    request->useGraph = useGraph;
    request->traveler = traveler;

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

size_t PathSubsystem::SplinePieceEnd(PointsArray const& points, size_t first)
{
    static constexpr float pieceRadius = 120.0f;
    if (first + 1 >= points.size())
        return first;
    size_t last = first + 1;
    while (last + 1 < points.size() && std::fabs(points[last + 1].x - points[first].x) <= pieceRadius &&
        std::fabs(points[last + 1].y - points[first].y) <= pieceRadius && std::fabs(points[last + 1].z - points[first].z) <= pieceRadius)
        ++last;
    return last;
}

void PathSubsystem::StartThreads()
{
    m_started = true;
    for (uint32 i = 0; i < WORKER_THREADS; ++i)
        m_threads.emplace_back(&PathSubsystem::Run, this);
}

void PathSubsystem::Shutdown()
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

void PathSubsystem::Run()
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

void PathSubsystem::Process(NavPathRequest& request)
{
    const bool sameMap = request.startMapId == request.destMapId;
    if (!request.useGraph && !sameMap)
    {
        request.state.store(NavPathState::Failed, std::memory_order_release);
        return;
    }

    // short: the navmesh alone
    NavPathSegment direct;
    direct.mapId = request.startMapId;
    PathType directType = PATHFIND_BLANK;
    if (sameMap && (!request.useGraph || (request.dest - request.start).length() <= SHORT_RANGE))
    {
        directType = BuildLeg(request.startMapId, request.start, request.dest, direct.points);
        if (!request.useGraph || Reaches(directType, direct.points, request.dest))
        {
            request.pathType = directType;
            if (directType & PATHFIND_NOPATH)
            {
                request.state.store(NavPathState::Failed, std::memory_order_release);
                return;
            }
            request.segments.push_back(std::move(direct));
            request.state.store(NavPathState::Ready, std::memory_order_release);
            return;
        }
    }

    // long: onto the travel graph, over it, off it
    RouteSearch search(GetGraph(), request);
    if (directType != PATHFIND_BLANK)
        search.ForbidDirect();
    if (search.Build(request.segments))
    {
        request.pathType = PATHFIND_NORMAL;
        request.viaGraph = true;
        request.state.store(NavPathState::Ready, std::memory_order_release);
        return;
    }

    // no route: on one map as far as the navmesh gets (the mover plans again where it ends)
    if (sameMap)
    {
        if (directType == PATHFIND_BLANK)
            directType = BuildLeg(request.startMapId, request.start, request.dest, direct.points);
        request.pathType = directType;
        if (!(directType & PATHFIND_NOPATH))
        {
            request.segments.clear();
            request.segments.push_back(std::move(direct));
            request.state.store(NavPathState::Ready, std::memory_order_release);
            return;
        }
    }
    request.segments.clear();
    request.state.store(NavPathState::Failed, std::memory_order_release);
}

#endif
