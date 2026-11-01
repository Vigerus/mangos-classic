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

#include "Combat/CombatWinChanceSubsystem.h"
#include "Entities/Player.h"
#include "Server/DBCStores.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include <algorithm>
#include <random>

namespace
{
    constexpr uint32 RUNS = 200;
    constexpr float STEP_SECONDS = 0.5f;
    constexpr float MAX_SECONDS = 120.0f;
    constexpr float GLOBAL_COOLDOWN_SECONDS = 1.5f;
    constexpr uint32 CACHE_MS = 5 * IN_MILLISECONDS;
    constexpr float ENEMY_RUN_SPEED = 7.0f;
    constexpr float MAX_HEAD_START_SECONDS = 5.0f;

    float WeaponDps(Unit* unit, uint16 minField, uint16 maxField, WeaponAttackType attack)
    {
        const uint32 attackTime = unit->GetAttackTime(attack);
        if (!attackTime)
            return 0.0f;
        return (unit->GetFloatValue(minField) + unit->GetFloatValue(maxField)) / 2.0f / (attackTime / 1000.0f);
    }

    // a hit's share left after the level difference (misses, glancing) and the armor (physical only)
    float DamageFactor(CombatSimUnit const& attacker, CombatSimUnit const& defender, bool physical)
    {
        const float levelFactor = std::clamp(1.0f + 0.05f * (int32(attacker.level) - int32(defender.level)), 0.5f, 1.5f);
        if (!physical)
            return levelFactor;
        const float armor = float(defender.armor);
        const float reduction = std::min(armor / (armor + 400.0f + 85.0f * attacker.level), 0.75f);
        return levelFactor * (1.0f - reduction);
    }

    bool CanCrowdControl(CombatSimUnit const& caster, CombatSimUnit const& target)
    {
        return caster.ccDurationMs && (!caster.ccTargetTypes || !target.creatureTypes || (caster.ccTargetTypes & target.creatureTypes));
    }

    uint64 Key(std::vector<CombatSimUnit> const& allies, std::vector<CombatSimUnit> const& enemies)
    {
        uint64 hash = 1469598103934665603ull;
        auto mix = [&hash](uint64 value) { hash = (hash ^ value) * 1099511628211ull; };
        for (auto const* team : { &allies, &enemies })
        {
            mix(team->size());
            for (CombatSimUnit const& unit : *team)
            {
                mix(unit.level);
                mix(unit.health / 10);
                mix(unit.mana / 10);
                mix(uint64(unit.meleeDps));
                mix(uint64(unit.spellDps));
                mix(unit.ccDurationMs);
                mix(uint64(unit.headStartSeconds));
            }
        }
        return hash;
    }
}

CombatSimUnit CombatSimUnit::Of(Unit* unit, float distance)
{
    CombatSimUnit sim;
    sim.level = unit->GetLevel();
    sim.health = unit->GetHealth();
    sim.armor = uint32(std::max(unit->GetArmor(), 0));
    sim.creatureTypes = unit->GetCreatureTypeMask();
    sim.meleeDps = WeaponDps(unit, UNIT_FIELD_MINDAMAGE, UNIT_FIELD_MAXDAMAGE, BASE_ATTACK);

    Player* player = unit->IsPlayer() ? static_cast<Player*>(unit) : nullptr;
    if (!player)
        return sim;

    bool ranged = false;
    if (player->hasRangedWeapon())
    {
        const float rangedDps = WeaponDps(unit, UNIT_FIELD_MINRANGEDDAMAGE, UNIT_FIELD_MAXRANGEDDAMAGE, RANGED_ATTACK);
        if (rangedDps > sim.meleeDps)
        {
            sim.meleeDps = rangedDps;
            ranged = true;
        }
    }

    sim.mana = player->GetPower(POWER_MANA);
    for (auto const& [spellId, playerSpell] : player->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled)
            continue;
        SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
        if (!spellInfo || IsPassiveSpell(spellInfo) || spellInfo->powerType != POWER_MANA)
            continue;

        float damage = 0.0f;
        bool crowdControl = false;
        for (uint32 e = 0; e < MAX_EFFECT_INDEX; ++e)
        {
            if (spellInfo->Effect[e] == SPELL_EFFECT_SCHOOL_DAMAGE)
                damage += spellInfo->EffectBasePoints[e] + spellInfo->EffectBaseDice[e] + spellInfo->EffectDieSides[e] / 2.0f;
            else if (spellInfo->Effect[e] == SPELL_EFFECT_APPLY_AURA && spellInfo->EffectApplyAuraName[e] == SPELL_AURA_MOD_CONFUSE)
                crowdControl = true;
        }

        // the crowd control that lasts longest (polymorph)
        if (crowdControl && !damage)
        {
            const int32 duration = GetSpellDuration(spellInfo);
            if (duration >= 8000 && uint32(duration) > sim.ccDurationMs)
            {
                sim.ccDurationMs = uint32(duration);
                sim.ccTargetTypes = spellInfo->TargetCreatureType;
            }
            continue;
        }

        // spammable direct damage: no cooldown
        if (damage <= 0.0f || spellInfo->RecoveryTime || spellInfo->CategoryRecoveryTime)
            continue;
        const float seconds = std::max(GetSpellCastTime(spellInfo, player) / 1000.0f, GLOBAL_COOLDOWN_SECONDS);
        if (damage / seconds > sim.spellDps)
        {
            sim.spellDps = damage / seconds;
            sim.manaPerSecond = Spell::CalculatePowerCost(spellInfo, player) / seconds;
            SpellRangeEntry const* range = sSpellRangeStore.LookupEntry(spellInfo->rangeIndex);
            ranged = ranged || (range && range->maxRange > 10.0f);
        }
    }

    if (ranged)
        sim.headStartSeconds = std::min(distance / ENEMY_RUN_SPEED, MAX_HEAD_START_SECONDS);
    return sim;
}

void CombatWinChanceSubsystem::Simulate(CombatSimRequest& request)
{
    std::vector<CombatSimUnit> const& allies = request.allies;
    std::vector<CombatSimUnit> const& enemies = request.enemies;
    if (allies.empty() || enemies.empty())
    {
        request.winChance = enemies.empty() ? 1.0f : 0.0f;
        request.healthLeftPct = enemies.empty() ? 100.0f : 0.0f;
        return;
    }

    std::mt19937 random(std::random_device{}());
    std::uniform_real_distribution<float> spread(0.8f, 1.2f);
    std::uniform_real_distribution<float> ccBreak(0.7f, 1.0f);

    float startHealth = 0.0f;
    float headStart = 0.0f;
    for (CombatSimUnit const& ally : allies)
    {
        startHealth += ally.health;
        headStart = std::max(headStart, ally.headStartSeconds);
    }

    uint32 wins = 0;
    float healthLeft = 0.0f;
    for (uint32 run = 0; run < RUNS; ++run)
    {
        std::vector<float> allyHealth, allyMana, allyBusyUntil;
        std::vector<int32> allyControls(allies.size(), -1);    // the enemy each ally keeps crowd-controlled
        for (CombatSimUnit const& ally : allies)
        {
            allyHealth.push_back(float(ally.health));
            allyMana.push_back(float(ally.mana));
            allyBusyUntil.push_back(0.0f);
        }
        std::vector<float> enemyHealth, controlledUntil(enemies.size(), 0.0f);
        std::vector<size_t> enemyTarget;
        for (CombatSimUnit const& enemy : enemies)
        {
            enemyHealth.push_back(float(enemy.health));
            enemyTarget.push_back(random() % allies.size());
        }

        auto alive = [](std::vector<float> const& health, size_t index) { return health[index] > 0.0f; };
        float time = 0.0f;
        for (; time < MAX_SECONDS; time += STEP_SECONDS)
        {
            std::vector<size_t> free;           // living enemies not crowd-controlled
            for (size_t e = 0; e < enemies.size(); ++e)
                if (alive(enemyHealth, e) && controlledUntil[e] <= time)
                    free.push_back(e);
            const bool anyEnemy = std::any_of(enemyHealth.begin(), enemyHealth.end(), [](float health) { return health > 0.0f; });
            const bool anyAlly = std::any_of(allyHealth.begin(), allyHealth.end(), [](float health) { return health > 0.0f; });
            if (!anyEnemy || !anyAlly)
                break;

            // focus: the weakest free enemy (a crowd-controlled one only when nothing else is left)
            int32 focus = -1;
            for (size_t e : free)
                if (focus < 0 || enemyHealth[e] < enemyHealth[focus])
                    focus = int32(e);
            if (focus < 0)
                for (size_t e = 0; e < enemies.size(); ++e)
                    if (alive(enemyHealth, e) && (focus < 0 || enemyHealth[e] < enemyHealth[focus]))
                        focus = int32(e);

            for (size_t a = 0; a < allies.size(); ++a)
            {
                if (!alive(allyHealth, a) || allyBusyUntil[a] > time)
                    continue;
                CombatSimUnit const& ally = allies[a];

                // crowd control: one more enemy than the one focused, none kept by this ally yet
                if (free.size() >= 2 && (allyControls[a] < 0 || controlledUntil[allyControls[a]] <= time || !alive(enemyHealth, allyControls[a])))
                {
                    int32 target = -1;
                    for (size_t e : free)
                        if (int32(e) != focus && CanCrowdControl(ally, enemies[e]) && (target < 0 || enemyHealth[e] > enemyHealth[target]))
                            target = int32(e);
                    if (target >= 0)
                    {
                        controlledUntil[target] = time + GLOBAL_COOLDOWN_SECONDS + ally.ccDurationMs / 1000.0f * ccBreak(random);
                        allyControls[a] = target;
                        allyBusyUntil[a] = time + GLOBAL_COOLDOWN_SECONDS;
                        free.erase(std::find(free.begin(), free.end(), size_t(target)));
                        continue;
                    }
                }

                if (focus < 0)
                    continue;
                const bool casting = ally.spellDps > 0.0f && allyMana[a] >= ally.manaPerSecond * STEP_SECONDS;
                float damage = (casting ? ally.spellDps : ally.meleeDps) * STEP_SECONDS * spread(random) * DamageFactor(ally, enemies[focus], !casting);
                if (casting)
                    allyMana[a] -= ally.manaPerSecond * STEP_SECONDS;
                enemyHealth[focus] -= damage;
                // damage breaks crowd control
                controlledUntil[focus] = std::min(controlledUntil[focus], time);
            }

            // the enemies hit once they ran in
            if (time < headStart)
                continue;
            for (size_t e = 0; e < enemies.size(); ++e)
            {
                if (!alive(enemyHealth, e) || controlledUntil[e] > time)
                    continue;
                if (!alive(allyHealth, enemyTarget[e]))
                {
                    std::vector<size_t> living;
                    for (size_t a = 0; a < allies.size(); ++a)
                        if (alive(allyHealth, a))
                            living.push_back(a);
                    if (living.empty())
                        break;
                    enemyTarget[e] = living[random() % living.size()];
                }
                const size_t target = enemyTarget[e];
                allyHealth[target] -= enemies[e].meleeDps * STEP_SECONDS * spread(random) * DamageFactor(enemies[e], allies[target], true);
            }
        }

        const bool won = std::none_of(enemyHealth.begin(), enemyHealth.end(), [](float health) { return health > 0.0f; }) &&
            std::any_of(allyHealth.begin(), allyHealth.end(), [](float health) { return health > 0.0f; });
        if (won)
        {
            ++wins;
            for (float health : allyHealth)
                healthLeft += std::max(health, 0.0f);
        }
    }

    request.winChance = float(wins) / RUNS;
    request.healthLeftPct = wins && startHealth > 0.0f ? healthLeft / wins / startHealth * 100.0f : 0.0f;
}

std::shared_ptr<CombatSimRequest> CombatWinChanceSubsystem::Request(std::vector<CombatSimUnit> allies, std::vector<CombatSimUnit> enemies)
{
    const uint64 key = Key(allies, enemies);
    const uint32 now = WorldTimer::getMSTime();
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        for (auto it = m_cache.begin(); it != m_cache.end();)
            it = now - it->second.second > CACHE_MS ? m_cache.erase(it) : std::next(it);
        auto cached = m_cache.find(key);
        if (cached != m_cache.end())
            return cached->second.first;
    }

    auto request = std::make_shared<CombatSimRequest>();
    request->allies = std::move(allies);
    request->enemies = std::move(enemies);
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_stop)
            return request;
        if (!m_thread.joinable())
            m_thread = std::thread(&CombatWinChanceSubsystem::Run, this);
        m_cache[key] = { request, now };
        m_queue.push_back(request);
    }
    m_wakeUp.notify_one();
    return request;
}

void CombatWinChanceSubsystem::Shutdown()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_stop = true;
        m_cache.clear();
    }
    m_wakeUp.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void CombatWinChanceSubsystem::Run()
{
    while (true)
    {
        std::shared_ptr<CombatSimRequest> request;
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

        Simulate(*request);
        request->state.store(CombatSimState::Ready, std::memory_order_release);
    }
}

#endif
