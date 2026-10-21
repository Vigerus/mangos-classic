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

#ifndef MANGOS_UNITRESOURCETHRESHOLDS_H
#define MANGOS_UNITRESOURCETHRESHOLDS_H

#include "delegates/Delegates.h"
#include "Globals/SharedDefines.h"

#include <memory>
#include <vector>

class Unit;

// how a resource percent is compared to a condition's percent
enum class EResourceCompare : uint8
{
    Lesser,
    LesserOrEqual,
    Equal,          // the same whole percent (rounded)
    GreaterOrEqual,
    Greater,
};

// a condition switched: true when it holds now, false when it stopped holding
DECLARE_MULTICAST_DELEGATE_OneParam(FOnResourceConditionSwitched, bool /*active*/);

/*
 * Health / power percent conditions of one unit, kept up to date by the unit's change broadcasts
 * (Unit::OnHealthChanged / OnPowerChanged) instead of being checked by whoever needs them:
 *
 *   UnitResourceThresholds thresholds;
 *   thresholds.Attach(unit);
 *   const auto& lowHealth = thresholds.WatchHealth(25.0f, EResourceCompare::Lesser);
 *   lowHealth.IsActive();                                                    // state, no calculation
 *   thresholds.WatchHealth(25.0f, EResourceCompare::Lesser).onSwitched.AddLambda([](bool active) { ... });
 *
 * Watching the same percent and comparison again returns the same condition. Conditions are checked on every change
 * of their resource, onSwitched fires only when the result changes. The owner must Detach (or destroy this) while
 * the unit still exists. Runs on the unit's map thread, like the broadcasts.
 */
class UnitResourceThresholds
{
    public:
        class Condition
        {
            public:
                bool IsActive() const { return m_active; }

                // fired when IsActive changes
                FOnResourceConditionSwitched onSwitched;

            private:
                friend class UnitResourceThresholds;

                Condition(bool health, Powers power, float percent, EResourceCompare compare)
                    : m_health(health), m_power(power), m_percent(percent), m_compare(compare) {}

                bool Holds(float percent) const;

                bool m_health;
                Powers m_power;             // when not m_health
                float m_percent;
                EResourceCompare m_compare;
                bool m_active = false;
        };

        UnitResourceThresholds() = default;
        ~UnitResourceThresholds() { Detach(); }
        UnitResourceThresholds(const UnitResourceThresholds&) = delete;
        UnitResourceThresholds& operator=(const UnitResourceThresholds&) = delete;

        // starts following unit's changes (conditions are checked against it at once)
        void Attach(Unit* unit);
        void Detach();
        Unit* GetUnit() const { return m_unit; }

        // health percent compared to percent
        Condition& WatchHealth(float percent, EResourceCompare compare);
        // percent of power (0 while the unit has no max power of that type) compared to percent
        Condition& WatchPower(Powers power, float percent, EResourceCompare compare);

    private:
        Condition& Watch(bool health, Powers power, float percent, EResourceCompare compare);

        void HandleHealthChanged(Unit* unit);
        void HandlePowerChanged(Unit* unit, Powers power);
        void Update(Condition& condition);

        float GetPercent(const Condition& condition) const;

        Unit* m_unit = nullptr;
        FDelegateHandle m_healthHandle;
        FDelegateHandle m_powerHandle;
        std::vector<std::unique_ptr<Condition>> m_conditions;    // stable addresses for the callers' references
};

#endif
