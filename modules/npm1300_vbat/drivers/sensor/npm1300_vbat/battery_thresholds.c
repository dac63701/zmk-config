/* SPDX-License-Identifier: MIT */

#include "battery_thresholds.h"

enum npm1300_battery_action npm1300_battery_step(
    struct npm1300_battery_state *state,
    const struct npm1300_battery_thresholds *thresholds,
    int32_t millivolts,
    bool vbus_present)
{
    enum npm1300_battery_action action = NPM1300_BATTERY_ACTION_NONE;

    /*
     * USB can supply the nonessential rail even when the cell is depleted.
     * Enforce that policy on every sample so plugging in also recovers a rail
     * that was previously disabled by the battery-only warning path.
     */
    if (vbus_present) {
        state->warning_active = false;
        state->critical_samples = 0;
        return NPM1300_BATTERY_ACTION_ENABLE_LOAD;
    }

    if (millivolts <= thresholds->warning_mv) {
        if (!state->warning_active) {
            state->warning_active = true;
            action = NPM1300_BATTERY_ACTION_DISABLE_LOAD;
        }
    } else if (millivolts >= thresholds->recovery_mv) {
        if (state->warning_active) {
            state->warning_active = false;
            action = NPM1300_BATTERY_ACTION_ENABLE_LOAD;
        }
        state->critical_samples = 0;
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
