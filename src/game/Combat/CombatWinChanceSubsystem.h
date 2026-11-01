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

#ifndef MANGOS_COMBAT_WIN_CHANCE_SUBSYSTEM_H
#define MANGOS_COMBAT_WIN_CHANCE_SUBSYSTEM_H

#ifdef ENABLE_PLAYERBOTS

#include "Common.h"
#include "World/WorldSubsystem.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Unit;

// a fighter as the simulation sees it: copied on the map thread, so the worker never touches a live unit
struct CombatSimUnit
{
    uint32 level = 1;
    uint32 health = 1;
    uint32 armor = 0;
    float meleeDps = 0.0f;          // melee (a creature's attack, a player's weapon or wand when the mana is gone)
    float spellDps = 0.0f;          // the best spammable direct damage spell, while the mana lasts
    float manaPerSecond = 0.0f;     // what that spell costs
    uint32 mana = 0;
    uint32 ccDurationMs = 0;        // a crowd-control spell it knows (polymorph), 0 without one
    uint32 ccTargetTypes = 0;       // the creature types that spell affects (0: any)
    uint32 creatureTypes = 0;       // its own creature type mask (for the other side's crowd control)
    float headStartSeconds = 0.0f;  // ranged: deals damage while the enemies run in

    // a player: its spells, weapon, mana; a creature: its melee damage; distance: how far the enemies start
    static CombatSimUnit Of(Unit* unit, float distance = 0.0f);
};

enum class CombatSimState : uint8
{
    Pending,
    Ready,
};

// A fight simulated by the CombatWinChanceSubsystem: read it on the requesting thread once IsDone().
struct CombatSimRequest
{
    std::vector<CombatSimUnit> allies;
    std::vector<CombatSimUnit> enemies;

    std::atomic<CombatSimState> state{ CombatSimState::Pending };
    float winChance = 0.0f;         // share of the runs the allies won, 0..1 (set before state turns Ready)
    float healthLeftPct = 0.0f;     // the allies' health left after the won runs, 0..100

    bool IsDone() const { return state.load(std::memory_order_acquire) == CombatSimState::Ready; }
};

// Estimates on its own thread how likely a team of players / bots beats a team of creatures: a rough fight in half
// second steps, run many times with random damage (+-20%) and random targets. Players focus the weakest enemy
// and cast their best spell while the mana lasts, then fall back to melee / wand; each creature hits one player; a
// player with a crowd-control spell takes one other enemy out of the fight while two or more are left. Not a combat
// log: no crits, resists, procs, cooldowns or fleeing. Results are kept 5 seconds for the same teams, so bots asking
// about one camp cost one simulation. Requests are polled (CombatSimRequest::IsDone); one nobody holds is skipped.
class CombatWinChanceSubsystem : public WorldSubsystem
{
    public:
        ~CombatWinChanceSubsystem() override { Shutdown(); }

        void Deinitialize() override { Shutdown(); }

        std::shared_ptr<CombatSimRequest> Request(std::vector<CombatSimUnit> allies, std::vector<CombatSimUnit> enemies);

        void Shutdown();

        // the simulation itself, on the calling thread
        static void Simulate(CombatSimRequest& request);

    private:
        void Run();

        std::mutex m_mutex;
        std::condition_variable m_wakeUp;
        std::deque<std::weak_ptr<CombatSimRequest>> m_queue;
        std::map<uint64, std::pair<std::shared_ptr<CombatSimRequest>, uint32>> m_cache;  // teams' key: request, when (ms)
        std::thread m_thread;
        bool m_stop = false;
};

#endif

#endif
