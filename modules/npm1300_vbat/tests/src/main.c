#include <zephyr/ztest.h>

#include "battery_thresholds.h"

static const struct npm1300_battery_thresholds thresholds = {
    .warning_mv = 3500,
    .ship_mv = 3200,
    .recovery_mv = 3600,
    .warning_confirm_samples = 3,
    .ship_confirm_samples = 3,
};

ZTEST(npm1300_vbat, test_warning_and_recovery_hysteresis)
{
    struct npm1300_battery_state state = {0};

    zassert_equal(npm1300_battery_step(&state, &thresholds, 3500, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3499, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_false(state.warning_active, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3499, false),
                  NPM1300_BATTERY_ACTION_DISABLE_LOAD, NULL);
    zassert_true(state.warning_active, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3599, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3600, false),
                  NPM1300_BATTERY_ACTION_ENABLE_LOAD, NULL);
    zassert_false(state.warning_active, NULL);
}

ZTEST(npm1300_vbat, test_ship_confirmation_and_vbus_deferral)
{
    struct npm1300_battery_state state = {0};

    zassert_equal(npm1300_battery_step(&state, &thresholds, 3200, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3199, true),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3199, true),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_false(state.warning_active, NULL);
    zassert_equal(state.critical_samples, 0, NULL);

    /* Unplugging restarts both load shedding and ship-mode confirmation. */
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3199, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3199, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3199, false),
                  NPM1300_BATTERY_ACTION_ENTER_SHIP, NULL);
}

ZTEST(npm1300_vbat, test_zero_voltage_with_vbus_never_enters_ship_mode)
{
    struct npm1300_battery_state state = {0};

    /*
     * A protected pack can present 0 V at its output after its UVP FETs open.
     * With USB attached, keep the PMIC active so its VBATLOW/trickle-charge
     * path can wake the protection circuit; ship mode would be counterproductive.
     */
    zassert_equal(npm1300_battery_step(&state, &thresholds, 0, true),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 0, true),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 0, true),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_false(state.warning_active, NULL);
    zassert_equal(state.critical_samples, 0, NULL);

    /* USB continues to enforce the nonessential rail even at a 0 V reading. */
    zassert_equal(npm1300_battery_step(&state, &thresholds, 0, true),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
}

ZTEST(npm1300_vbat, test_implausible_voltage_never_enters_ship_mode_unplugged)
{
    struct npm1300_battery_state state = {0};

    for (int i = 0; i < 10; i++) {
        zassert_equal(npm1300_battery_step(&state, &thresholds, 0, false),
                      NPM1300_BATTERY_ACTION_NONE, NULL);
    }

    zassert_false(state.warning_active, NULL);
    zassert_equal(state.warning_samples, 0, NULL);
    zassert_equal(state.critical_samples, 0, NULL);
}

ZTEST(npm1300_vbat, test_vbus_clears_cutoff_without_forcing_load_on)
{
    struct npm1300_battery_state state = {0};

    zassert_equal(npm1300_battery_step(&state, &thresholds, 3400, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3400, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3400, false),
                  NPM1300_BATTERY_ACTION_DISABLE_LOAD, NULL);
    zassert_true(state.warning_active, NULL);

    zassert_equal(npm1300_battery_step(&state, &thresholds, 3400, true),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_false(state.warning_active, NULL);
    zassert_equal(state.critical_samples, 0, NULL);
}

ZTEST(npm1300_vbat, test_voltage_above_ship_resets_confirmation)
{
    struct npm1300_battery_state state = {0};

    zassert_equal(npm1300_battery_step(&state, &thresholds, 3100, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3250, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(state.critical_samples, 0, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3200, false),
                  NPM1300_BATTERY_ACTION_DISABLE_LOAD, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3200, false),
                  NPM1300_BATTERY_ACTION_NONE, NULL);
    zassert_equal(npm1300_battery_step(&state, &thresholds, 3200, false),
                  NPM1300_BATTERY_ACTION_ENTER_SHIP, NULL);
}

ZTEST_SUITE(npm1300_vbat, NULL, NULL, NULL, NULL, NULL);
