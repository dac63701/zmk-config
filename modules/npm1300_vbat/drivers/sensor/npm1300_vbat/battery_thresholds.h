/* SPDX-License-Identifier: MIT */

#ifndef ZMK_NPM1300_BATTERY_THRESHOLDS_H_
#define ZMK_NPM1300_BATTERY_THRESHOLDS_H_

#include <stdbool.h>
#include <stdint.h>

struct npm1300_battery_thresholds {
    int32_t warning_mv;
    int32_t ship_mv;
    int32_t recovery_mv;
    uint8_t ship_confirm_samples;
};

struct npm1300_battery_state {
    uint8_t critical_samples;
    bool warning_active;
};

enum npm1300_battery_action {
    NPM1300_BATTERY_ACTION_NONE,
    NPM1300_BATTERY_ACTION_DISABLE_LOAD,
    NPM1300_BATTERY_ACTION_ENABLE_LOAD,
    NPM1300_BATTERY_ACTION_ENTER_SHIP,
};

enum npm1300_battery_action npm1300_battery_step(
    struct npm1300_battery_state *state,
    const struct npm1300_battery_thresholds *thresholds,
    int32_t millivolts,
    bool vbus_present);

#endif
