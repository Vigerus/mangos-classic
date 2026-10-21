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

#ifndef MANGOS_PROFILINGSUBSYSTEM_H
#define MANGOS_PROFILINGSUBSYSTEM_H

#include "World/WorldSubsystem.h"

#include <atomic>
#include <chrono>
#include <string>

class Player;

// a scope name (TRACE_CPU_PROFILER_ENTRY_SCOPE argument or a runtime name), one per name for the whole process
struct ProfilingStat
{
    ProfilingStat(std::string name, uint32 id) : name(std::move(name)), id(id) {}

    std::string const name;
    uint32 const id;
};

// a call path of one thread: the stat run inside the parent's stat. Written only by its thread (no atomic
// read-modify-write), read by the report; children are published lock free (never removed).
struct ProfilingNode
{
    ProfilingNode(ProfilingStat* stat, ProfilingNode* parent) : stat(stat), parent(parent) {}

    void Add(uint64 ns);

    ProfilingStat* const stat;                      // nullptr for the root of a thread
    ProfilingNode* const parent;
    std::atomic<ProfilingNode*> firstChild{nullptr};
    std::atomic<ProfilingNode*> nextSibling{nullptr};

    // values of the reset period epoch (ProfilingSubsystem::ResetStats starts a new one)
    std::atomic<uint32> epoch{0};
    std::atomic<uint64> count{0};
    std::atomic<uint64> totalNs{0};
    std::atomic<uint64> maxNs{0};
};

// times its stat from its construction to its destruction, as a child of the enclosing scope of the thread
class ProfilingScope
{
    public:
        explicit ProfilingScope(ProfilingStat* stat);
        ~ProfilingScope();

        ProfilingScope(ProfilingScope const&) = delete;
        ProfilingScope& operator=(ProfilingScope const&) = delete;

    private:
        ProfilingNode* m_node;
        std::chrono::steady_clock::time_point m_start;
};

#define MANGOS_PROFILER_CONCAT_INNER(a, b) a##b
#define MANGOS_PROFILER_CONCAT(a, b) MANGOS_PROFILER_CONCAT_INNER(a, b)

/*
 * Times the rest of the enclosing scope (Unreal style), in any thread:
 *
 *   void StateMachine::Update(uint32 elapsed, bool minimal)
 *   {
 *       TRACE_CPU_PROFILER_ENTRY_SCOPE(StateMachine::Update);
 *       ...
 *
 * Name is the text of the argument; scopes with the same name share their stat. The stat is looked up once per
 * call site (function local static). Scopes nest per thread: the report is a call tree with inclusive and exclusive
 * times per path, plus a flat list per name.
 */
#define TRACE_CPU_PROFILER_ENTRY_SCOPE(Name) \
    static ProfilingStat* const MANGOS_PROFILER_CONCAT(profilingStat_, __LINE__) = ProfilingSubsystem::RegisterStat(#Name); \
    ProfilingScope const MANGOS_PROFILER_CONCAT(profilingScope_, __LINE__)(MANGOS_PROFILER_CONCAT(profilingStat_, __LINE__))

// same with a stat of a runtime name (ProfilingSubsystem::RegisterStat(std::string), cache the pointer: the lookup
// takes a lock), e.g. one per trigger / action / value of the bot AI
#define TRACE_CPU_PROFILER_STAT_SCOPE(Stat) \
    ProfilingScope const MANGOS_PROFILER_CONCAT(profilingScope_, __LINE__)(Stat)

/*
 * Collects the TRACE_CPU_PROFILER_ENTRY_SCOPE timings since the server started (or the last ResetStats). The stats
 * and call trees live as long as the process, so scopes may run before World registered the subsystem and after it
 * is destroyed.
 *
 * BotStateTree addon (Telemetry tab): the addon message "BOT\ttelemetry" on any chat channel, also without a group,
 * is answered at once with the report; "BOT\ttelemetry reset" zeroes the stats first.
 */
class ProfilingSubsystem : public WorldSubsystem
{
    public:
        // the stat of a name, created on its first use; thread safe (takes a lock)
        static ProfilingStat* RegisterStat(std::string const& name);
        static ProfilingStat* RegisterStat(char const* name) { return RegisterStat(std::string(name)); }
        // starts a new period: every value counts from zero (lock free, scopes running meanwhile are not lost)
        static void ResetStats();
        // ms since the server started or the last ResetStats
        static uint64 GetCollectingMs();
        // the data of the addon report as text: call tree with the paths of the most inclusive time, then the names
        // of the most exclusive time (count, inclusive / exclusive / max ms), e.g. for the automation test logs
        static std::string BuildTextReport(size_t maxTreeNodes = 80, size_t maxFlatStats = 40);

        // the addon request (see above), from the chat handler. True when msg was the request.
        bool HandleReportRequest(Player* player, std::string const& msg);

    private:
        // "BOTPF\tS<seq>:<index>/<count>:<payload part>" addon messages to the player, records joined by '~':
        //   P^<ms collected>^<nodes sent>^<nodes total>
        //   S^<stat id>^<name>                       names of the stats used below
        //   N^<id>^<parent id, 0 top level>^<stat id>^<count>^<inclusive ns>^<exclusive ns>^<max ns>^<hidden children>^<hidden inclusive ns>
        //                                            call tree merged over the threads, depth first, children by
        //                                            inclusive time; the nodes with the most inclusive time (and
        //                                            their parents) are sent, the others are summed as hidden
        //   F^<stat id>^<count>^<inclusive ns>^<exclusive ns>^<max ns>
        //                                            per name over every path (inclusive without recursion), the
        //                                            names with the most exclusive time
        void SendReport(Player* player);

        std::atomic<uint32> m_reportSeq{0};
};

#endif
