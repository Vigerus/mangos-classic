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

#include "World/ProfilingSubsystem.h"
#include "Entities/Player.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Chat/Chat.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // nodes sent in the call tree, names in the flat list (the addon message report stays a few dozen KB)
    constexpr size_t reportTreeNodes = 250;
    constexpr size_t reportFlatStats = 150;

    int64 NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // the call tree of one thread
    struct ProfilingThread
    {
        // (parent, stat) -> child lookups, direct mapped: most scopes find their node without walking the children
        struct CacheEntry
        {
            ProfilingNode* parent = nullptr;
            ProfilingStat* stat = nullptr;
            ProfilingNode* node = nullptr;
        };
        static constexpr size_t cacheSize = 1024;

        ProfilingNode root{nullptr, nullptr};
        ProfilingNode* current = &root;
        CacheEntry cache[cacheSize];

        // the child of the current node for stat, created on its first use; becomes the current node
        ProfilingNode* Enter(ProfilingStat* stat)
        {
            ProfilingNode* parent = current;
            CacheEntry& entry = cache[((reinterpret_cast<uintptr_t>(parent) >> 4) * 31 + (reinterpret_cast<uintptr_t>(stat) >> 4)) & (cacheSize - 1)];
            if (entry.parent != parent || entry.stat != stat)
            {
                ProfilingNode* node = parent->firstChild.load(std::memory_order_relaxed);
                while (node && node->stat != stat)
                    node = node->nextSibling.load(std::memory_order_relaxed);
                if (!node)
                {
                    // published for the report thread once complete
                    node = new ProfilingNode(stat, parent);
                    node->nextSibling.store(parent->firstChild.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    parent->firstChild.store(node, std::memory_order_release);
                }
                entry.parent = parent;
                entry.stat = stat;
                entry.node = node;
            }
            current = entry.node;
            return entry.node;
        }
    };

    struct ProfilingRegistry
    {
        std::mutex lock;
        std::vector<std::unique_ptr<ProfilingStat>> stats;              // by id, never removed
        std::unordered_map<std::string, ProfilingStat*> statsByName;
        std::vector<ProfilingThread*> threads;                          // never removed (a thread pool's trees stay)
        std::atomic<uint32> epoch{1};
        std::atomic<int64> startNs{NowNs()};
    };

    // never destroyed: scopes may still run while static objects are destroyed at exit
    ProfilingRegistry& GetRegistry()
    {
        static ProfilingRegistry* registry = new ProfilingRegistry();
        return *registry;
    }

    ProfilingThread& GetThread()
    {
        thread_local ProfilingThread* thread = nullptr;
        if (!thread)
        {
            thread = new ProfilingThread();
            ProfilingRegistry& registry = GetRegistry();
            std::lock_guard<std::mutex> lock(registry.lock);
            registry.threads.push_back(thread);
        }
        return *thread;
    }

    // the call trees of every thread merged by path
    struct MergedNode
    {
        ProfilingStat* stat = nullptr;
        MergedNode* parent = nullptr;
        uint64 count = 0;
        uint64 totalNs = 0;
        uint64 maxNs = 0;
        uint64 exclusiveNs = 0;
        std::map<uint32, std::unique_ptr<MergedNode>> children;     // by stat id
        bool sent = false;
    };

    void Merge(ProfilingNode const& node, MergedNode& merged, uint32 epoch)
    {
        for (ProfilingNode const* child = node.firstChild.load(std::memory_order_acquire); child; child = child->nextSibling.load(std::memory_order_acquire))
        {
            std::unique_ptr<MergedNode>& target = merged.children[child->stat->id];
            if (!target)
            {
                target = std::make_unique<MergedNode>();
                target->stat = child->stat;
                target->parent = &merged;
            }
            if (child->epoch.load(std::memory_order_relaxed) == epoch)
            {
                target->count += child->count.load(std::memory_order_relaxed);
                target->totalNs += child->totalNs.load(std::memory_order_relaxed);
                target->maxNs = std::max(target->maxNs, child->maxNs.load(std::memory_order_relaxed));
            }
            Merge(*child, *target, epoch);
        }
    }

    struct FlatStat
    {
        ProfilingStat* stat = nullptr;
        uint64 count = 0;
        uint64 totalNs = 0;
        uint64 exclusiveNs = 0;
        uint64 maxNs = 0;
    };

    // drops the paths that did not run this period, computes the exclusive times, collects the nodes and the
    // per name sums (inclusive only where the name is not already on the path: recursion is counted once)
    void Finish(MergedNode& node, std::vector<MergedNode*>& nodes, std::unordered_map<uint32, FlatStat>& flat, std::vector<ProfilingStat*>& path)
    {
        uint64 childrenNs = 0;
        for (auto itr = node.children.begin(); itr != node.children.end();)
        {
            MergedNode& child = *itr->second;
            if (!child.count)
            {
                itr = node.children.erase(itr);
                continue;
            }

            bool const recursive = std::find(path.begin(), path.end(), child.stat) != path.end();
            path.push_back(child.stat);
            Finish(child, nodes, flat, path);
            path.pop_back();

            FlatStat& stat = flat[child.stat->id];
            stat.stat = child.stat;
            stat.count += child.count;
            stat.exclusiveNs += child.exclusiveNs;
            stat.maxNs = std::max(stat.maxNs, child.maxNs);
            if (!recursive)
                stat.totalNs += child.totalNs;

            childrenNs += child.totalNs;
            nodes.push_back(&child);
            ++itr;
        }
        node.exclusiveNs = node.totalNs > childrenNs ? node.totalNs - childrenNs : 0;
    }

    // names go between '^' / '~' separators of an addon message: no separators, no chat escapes
    std::string ReportName(std::string name)
    {
        for (char& c : name)
            if (c == '^' || c == '~' || c == '|' || c == '\n' || c == '\r' || c == '\t')
                c = '_';
        return name;
    }

    // the merged call tree of the period, the nodes with the most inclusive time (and their parents) marked sent,
    // the names with the most exclusive time
    struct ProfilingSnapshot
    {
        MergedNode root;
        std::vector<MergedNode*> nodes;             // by inclusive time
        std::vector<FlatStat> flatStats;
        size_t sentCount = 0;
    };

    void TakeSnapshot(ProfilingSnapshot& snapshot, size_t maxTreeNodes, size_t maxFlatStats)
    {
        ProfilingRegistry& registry = GetRegistry();
        std::vector<ProfilingThread*> threads;
        {
            std::lock_guard<std::mutex> lock(registry.lock);
            threads = registry.threads;
        }

        uint32 const epoch = registry.epoch.load(std::memory_order_relaxed);
        for (ProfilingThread const* thread : threads)
            Merge(thread->root, snapshot.root, epoch);

        std::unordered_map<uint32, FlatStat> flat;
        std::vector<ProfilingStat*> path;
        Finish(snapshot.root, snapshot.nodes, flat, path);

        std::sort(snapshot.nodes.begin(), snapshot.nodes.end(), [](MergedNode const* a, MergedNode const* b) { return a->totalNs > b->totalNs; });
        for (MergedNode* node : snapshot.nodes)
        {
            if (snapshot.sentCount >= maxTreeNodes)
                break;
            for (MergedNode* itr = node; itr && itr->stat && !itr->sent; itr = itr->parent)
            {
                itr->sent = true;
                ++snapshot.sentCount;
            }
        }

        for (auto const& itr : flat)
            snapshot.flatStats.push_back(itr.second);
        std::sort(snapshot.flatStats.begin(), snapshot.flatStats.end(), [](FlatStat const& a, FlatStat const& b) { return a.exclusiveNs > b.exclusiveNs; });
        if (snapshot.flatStats.size() > maxFlatStats)
            snapshot.flatStats.resize(maxFlatStats);
    }

    std::string FormatMs(uint64 ns)
    {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.3f", double(ns) / 1000000.0);
        return buffer;
    }

    void WriteTextTree(MergedNode const& node, uint32 depth, std::ostringstream& out)
    {
        std::vector<MergedNode const*> children;
        for (auto const& itr : node.children)
            if (itr.second->sent)
                children.push_back(itr.second.get());
        std::sort(children.begin(), children.end(), [](MergedNode const* a, MergedNode const* b) { return a->totalNs > b->totalNs; });

        for (MergedNode const* child : children)
        {
            char line[128];
            snprintf(line, sizeof(line), "%10llu %12s %12s %10s  ", (unsigned long long)child->count, FormatMs(child->totalNs).c_str(),
                FormatMs(child->exclusiveNs).c_str(), FormatMs(child->maxNs).c_str());
            out << line << std::string(depth * 2, ' ') << child->stat->name << "\n";
            WriteTextTree(*child, depth + 1, out);
        }
    }

    void WriteTree(MergedNode const& node, uint32 parentId, uint32& nextId, std::ostringstream& out)
    {
        std::vector<MergedNode const*> children;
        for (auto const& itr : node.children)
            children.push_back(itr.second.get());
        std::sort(children.begin(), children.end(), [](MergedNode const* a, MergedNode const* b) { return a->totalNs > b->totalNs; });

        // the hidden children of a sent node: summed on it
        uint32 hiddenCount = 0;
        uint64 hiddenNs = 0;
        for (MergedNode const* child : children)
        {
            if (!child->sent)
            {
                ++hiddenCount;
                hiddenNs += child->totalNs;
            }
        }

        uint32 id = 0;
        if (node.stat)
        {
            id = ++nextId;
            out << "~N^" << id << "^" << parentId << "^" << node.stat->id << "^" << node.count << "^" << node.totalNs << "^" << node.exclusiveNs
                << "^" << node.maxNs << "^" << hiddenCount << "^" << hiddenNs;
        }

        for (MergedNode const* child : children)
            if (child->sent)
                WriteTree(*child, id, nextId, out);
    }
}

void ProfilingNode::Add(uint64 ns)
{
    // only the node's thread writes: plain loads / stores, a new period starts from zero
    uint32 const currentEpoch = GetRegistry().epoch.load(std::memory_order_relaxed);
    if (epoch.load(std::memory_order_relaxed) != currentEpoch)
    {
        count.store(0, std::memory_order_relaxed);
        totalNs.store(0, std::memory_order_relaxed);
        maxNs.store(0, std::memory_order_relaxed);
        epoch.store(currentEpoch, std::memory_order_relaxed);
    }
    count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    totalNs.store(totalNs.load(std::memory_order_relaxed) + ns, std::memory_order_relaxed);
    if (ns > maxNs.load(std::memory_order_relaxed))
        maxNs.store(ns, std::memory_order_relaxed);
}

ProfilingScope::ProfilingScope(ProfilingStat* stat) : m_node(GetThread().Enter(stat)), m_start(std::chrono::steady_clock::now())
{
}

ProfilingScope::~ProfilingScope()
{
    m_node->Add(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count()));
    GetThread().current = m_node->parent;
}

ProfilingStat* ProfilingSubsystem::RegisterStat(std::string const& name)
{
    ProfilingRegistry& registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.lock);
    auto itr = registry.statsByName.find(name);
    if (itr != registry.statsByName.end())
        return itr->second;

    registry.stats.push_back(std::make_unique<ProfilingStat>(name, uint32(registry.stats.size() + 1)));
    ProfilingStat* stat = registry.stats.back().get();
    registry.statsByName.emplace(name, stat);
    return stat;
}

void ProfilingSubsystem::ResetStats()
{
    ProfilingRegistry& registry = GetRegistry();
    registry.startNs = NowNs();
    ++registry.epoch;
}

uint64 ProfilingSubsystem::GetCollectingMs()
{
    return uint64(std::max<int64>(0, NowNs() - GetRegistry().startNs)) / 1000000;
}

std::string ProfilingSubsystem::BuildTextReport(size_t maxTreeNodes, size_t maxFlatStats)
{
    ProfilingSnapshot snapshot;
    TakeSnapshot(snapshot, maxTreeNodes, maxFlatStats);

    std::ostringstream out;
    out << "Profiling: " << GetCollectingMs() << " ms collected, " << snapshot.sentCount << " of " << snapshot.nodes.size() << " call paths shown\n";
    out << "Call tree (by inclusive time):\n";
    out << "     calls      incl ms      excl ms     max ms  name\n";
    WriteTextTree(snapshot.root, 0, out);

    out << "Flat (by exclusive time, inclusive without recursion):\n";
    out << "     calls      incl ms      excl ms     max ms  name\n";
    for (FlatStat const& stat : snapshot.flatStats)
    {
        char line[128];
        snprintf(line, sizeof(line), "%10llu %12s %12s %10s  ", (unsigned long long)stat.count, FormatMs(stat.totalNs).c_str(),
            FormatMs(stat.exclusiveNs).c_str(), FormatMs(stat.maxNs).c_str());
        out << line << stat.stat->name << "\n";
    }
    return out.str();
}

bool ProfilingSubsystem::HandleReportRequest(Player* player, std::string const& msg)
{
    std::string const request = "BOT\ttelemetry";
    if (msg.rfind(request, 0) != 0)
        return false;

    if (msg.compare(request.size(), std::string::npos, " reset") == 0)
        ResetStats();

    if (player)
        SendReport(player);
    return true;
}

void ProfilingSubsystem::SendReport(Player* player)
{
    if (!player->GetSession())
        return;

    ProfilingSnapshot snapshot;
    TakeSnapshot(snapshot, reportTreeNodes, reportFlatStats);
    MergedNode const& root = snapshot.root;
    std::vector<MergedNode*> const& nodes = snapshot.nodes;
    std::vector<FlatStat> const& flatStats = snapshot.flatStats;
    size_t const sentCount = snapshot.sentCount;

    std::ostringstream payload;
    payload << "P^" << GetCollectingMs() << "^" << sentCount << "^" << nodes.size();

    // names of the sent nodes and flat stats, once each
    std::unordered_set<uint32> named;
    auto writeName = [&](ProfilingStat const* stat)
    {
        if (named.insert(stat->id).second)
            payload << "~S^" << stat->id << "^" << ReportName(stat->name);
    };
    for (MergedNode const* node : nodes)
        if (node->sent)
            writeName(node->stat);
    for (FlatStat const& stat : flatStats)
        writeName(stat.stat);

    uint32 nextId = 0;
    WriteTree(root, 0, nextId, payload);

    for (FlatStat const& stat : flatStats)
        payload << "~F^" << stat.stat->id << "^" << stat.count << "^" << stat.totalNs << "^" << stat.exclusiveNs << "^" << stat.maxNs;

    // addon messages are limited to 255 chars: chunks, sent as party addon messages from the player itself
    size_t const chunkSize = 220;
    std::string const text = payload.str();
    size_t const count = std::max<size_t>(1, (text.size() + chunkSize - 1) / chunkSize);
    uint32 const seq = ++m_reportSeq % 256;
    for (size_t i = 0; i < count; ++i)
    {
        std::ostringstream chunk;
        chunk << "BOTPF\tS" << seq << ":" << (i + 1) << "/" << count << ":" << text.substr(i * chunkSize, chunkSize);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, chunk.str().c_str(), LANG_ADDON, CHAT_TAG_NONE, player->GetObjectGuid(), player->GetName());
        player->GetSession()->SendPacket(data);
    }
}
