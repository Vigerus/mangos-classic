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

#include "Entities/UnitResourceThresholds.h"
#include "Entities/Unit.h"

#include <cmath>

bool UnitResourceThresholds::Condition::Holds(float percent) const
{
    switch (m_compare)
    {
        case EResourceCompare::Lesser:          return percent < m_percent;
        case EResourceCompare::LesserOrEqual:   return percent <= m_percent;
        case EResourceCompare::Equal:           return std::lround(percent) == std::lround(m_percent);
        case EResourceCompare::GreaterOrEqual:  return percent >= m_percent;
        case EResourceCompare::Greater:         return percent > m_percent;
    }
    return false;
}

void UnitResourceThresholds::Attach(Unit* unit)
{
    if (m_unit == unit)
        return;

    Detach();
    m_unit = unit;
    if (!m_unit)
        return;

    m_healthHandle = m_unit->OnHealthChanged.Add(this, &UnitResourceThresholds::HandleHealthChanged);
    m_powerHandle = m_unit->OnPowerChanged.Add(this, &UnitResourceThresholds::HandlePowerChanged);

    for (size_t i = 0; i < m_conditions.size(); ++i)
        Update(*m_conditions[i]);
}

void UnitResourceThresholds::Detach()
{
    if (!m_unit)
        return;

    m_unit->OnHealthChanged.Remove(m_healthHandle);
    m_unit->OnPowerChanged.Remove(m_powerHandle);
    m_healthHandle.Reset();
    m_powerHandle.Reset();
    m_unit = nullptr;
}

UnitResourceThresholds::Condition& UnitResourceThresholds::WatchHealth(float percent, EResourceCompare compare)
{
    return Watch(true, POWER_MANA, percent, compare);
}

UnitResourceThresholds::Condition& UnitResourceThresholds::WatchPower(Powers power, float percent, EResourceCompare compare)
{
    return Watch(false, power, percent, compare);
}

UnitResourceThresholds::Condition& UnitResourceThresholds::Watch(bool health, Powers power, float percent, EResourceCompare compare)
{
    for (std::unique_ptr<Condition>& condition : m_conditions)
        if (condition->m_health == health && (health || condition->m_power == power) &&
            condition->m_percent == percent && condition->m_compare == compare)
            return *condition;

    m_conditions.emplace_back(new Condition(health, power, percent, compare));
    Condition& condition = *m_conditions.back();
    // the starting state, without a switch broadcast (nobody subscribed yet)
    if (m_unit)
        condition.m_active = condition.Holds(GetPercent(condition));
    return condition;
}

void UnitResourceThresholds::HandleHealthChanged(Unit* /*unit*/)
{
    // by index: a switch callback may watch a new condition
    for (size_t i = 0; i < m_conditions.size(); ++i)
        if (m_conditions[i]->m_health)
            Update(*m_conditions[i]);
}

void UnitResourceThresholds::HandlePowerChanged(Unit* /*unit*/, Powers power)
{
    // by index: a switch callback may watch a new condition
    for (size_t i = 0; i < m_conditions.size(); ++i)
        if (!m_conditions[i]->m_health && m_conditions[i]->m_power == power)
            Update(*m_conditions[i]);
}

void UnitResourceThresholds::Update(Condition& condition)
{
    const bool active = condition.Holds(GetPercent(condition));
    if (active == condition.m_active)
        return;

    condition.m_active = active;
    condition.onSwitched.Broadcast(active);
}

float UnitResourceThresholds::GetPercent(const Condition& condition) const
{
    if (condition.m_health)
        return m_unit->GetMaxHealth() ? m_unit->GetHealthPercent() : 0.0f;

    return m_unit->GetPowerPercent(condition.m_power);
}
