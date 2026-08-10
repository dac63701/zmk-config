/* SPDX-License-Identifier: MIT */

#include "battery_thresholds.h"

bool npm1300_battery_voltage_is_plausible(int32_t millivolts)
{
    return millivolts >= NPM1300_BATTERY_PLAUSIBLE_MIN_MV &&
           millivolts <= NPM1300_BATTERY_PLAUSIBLE_MAX_MV;
}

enum npm1300_battery_action npm1300_battery_step(
    struct npm1300_battery_state *state,
    const struct npm1300_battery_thresholds *thresholds,
    int32_t millivolts,
    bool vbus_present)
{
    enum npm1300_battery_action action = NPM1300_BATTERY_ACTION_NONE;

    /* USB owns the handoff; the RGB driver is the only component that enables its rail. */
    if (vbus_present) {
        state->warning_active = false;
        state->warning_samples = 0;
        state->critical_samples = 0;
        return NPM1300_BATTERY_ACTION_NONE;
    }

    if (!npm1300_battery_voltage_is_plausible(millivolts)) {
        state->warning_samples = 0;
        state->critical_samples = 0;
        return NPM1300_BATTERY_ACTION_NONE;
    }

    if (millivolts <= thresholds->warning_mv) {
        if (!state->warning_active) {
            if (state->warning_samples < thresholds->warning_confirm_samples) {
                state->warning_samples++;
            }
            if (state->warning_samples >= thresholds->warning_confirm_samples) {
                state->warning_active = true;
                action = NPM1300_BATTERY_ACTION_DISABLE_LOAD;
            }
        }
    } else {
        state->warning_samples = 0;
        if (millivolts >= thresholds->recovery_mv && state->warning_active) {
            state->warning_active = false;
            action = NPM1300_BATTERY_ACTION_ENABLE_LOAD;
        }
    }

    if (millivolts <= thresholds->ship_mv) {
        if (state->critical_samples < thresholds->ship_confirm_samples) {
            state->critical_samples++;
        }
        if (state->critical_samples >= thresholds->ship_confirm_samples) {
            return NPM1300_BATTERY_ACTION_ENTER_SHIP;
        }
    } else {
        state->critical_samples = 0;
    }

    return action;
}
