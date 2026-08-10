/*
 * Copyright (c) 2025 Oleksandr Maslov
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_npm1300_vbat

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mfd/npm1300.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <drivers/ext_power.h>
#include <nrf_fuel_gauge.h>
#include <zmk/activity.h>
#include <zmk/usb.h>

#include "battery_thresholds.h"

LOG_MODULE_REGISTER(npm1300_vbat, CONFIG_SENSOR_LOG_LEVEL);

/* Nordic nPMx TASKENTERSHIPMODE: SHPHLD base 0x0b, offset 0x02. */
#define NPM1300_SHPHLD_BASE            0x0bU
#define NPM1300_TASK_ENTER_SHIP_OFFSET 0x02U

#if defined(CONFIG_FPU_SHARING)
#define NPM1300_MONITOR_THREAD_OPTIONS K_FP_REGS
#else
#define NPM1300_MONITOR_THREAD_OPTIONS 0
#endif

static const struct battery_model battery_model = {
#include "battery_model.inc"
};

struct npm1300_vbat_config {
    const struct device *charger;
    const struct device *pmic;
    const struct device *ext_power;
};

struct npm1300_measurement {
    int32_t millivolts;
    float voltage;
    float current;
    bool vbus_present;
};

struct npm1300_vbat_data {
    const struct device *dev;
    struct k_thread monitor_thread;
    K_KERNEL_STACK_MEMBER(monitor_stack, CONFIG_ZMK_NPM1300_FUEL_GAUGE_STACK_SIZE);
    struct npm1300_battery_state battery_state;
    int64_t last_fuel_gauge_update_ms;
    int64_t last_protection_update_ms;
    int32_t filtered_mv;
    atomic_t state_of_charge;
    atomic_t fuel_gauge_valid;
    bool filtered_voltage_valid;
    bool fuel_gauge_initialized;
};

static void npm1300_update_filtered_voltage(struct npm1300_vbat_data *data, int32_t millivolts)
{
    if (!data->filtered_voltage_valid) {
        data->filtered_mv = millivolts;
        data->filtered_voltage_valid = true;
        return;
    }

    /* Keep voltage available for diagnostics without using it as the reported percentage. */
    data->filtered_mv = (data->filtered_mv * 3 + millivolts + 2) / 4;
}

static int npm1300_vbus_present(const struct npm1300_vbat_config *config, bool *present)
{
    struct sensor_value value;
    int ret = sensor_attr_get(config->charger, SENSOR_CHAN_NPM1300_CHARGER_VBUS_STATUS,
                              SENSOR_ATTR_NPM1300_CHARGER_VBUS_PRESENT, &value);

    if (ret == 0) {
        *present = value.val1 != 0;
    }

    return ret;
}

static int npm1300_read_measurement(const struct npm1300_vbat_config *config,
                                    struct npm1300_measurement *measurement)
{
    struct sensor_value value;
    int ret = sensor_sample_fetch(config->charger);

    if (ret != 0) {
        return ret;
    }

    ret = sensor_channel_get(config->charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
    if (ret != 0) {
        return ret;
    }

    measurement->voltage = sensor_value_to_float(&value);
    measurement->millivolts = value.val1 * 1000 + value.val2 / 1000;

    ret = sensor_channel_get(config->charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
    if (ret != 0) {
        return ret;
    }

    /*
     * The Zephyr 3.5 nPM1300 driver used by this board reports positive current
     * while discharging and negative current while charging. That is the sign
     * convention expected by the matching nRF Fuel Gauge library.
     */
    measurement->current = sensor_value_to_float(&value);

    bool pmic_vbus_present = false;
    ret = npm1300_vbus_present(config, &pmic_vbus_present);
    if (ret != 0) {
        return ret;
    }

    measurement->vbus_present = zmk_usb_is_powered() || pmic_vbus_present;
    return 0;
}

static void npm1300_disable_nonessential_loads(const struct npm1300_vbat_config *config)
{
    if (config->ext_power != NULL && device_is_ready(config->ext_power)) {
        int ret = ext_power_disable(config->ext_power);

        if (ret != 0) {
            LOG_ERR("Failed to disable nonessential load: %d", ret);
        }
    }
}

static int npm1300_enter_ship_mode(const struct npm1300_vbat_config *config)
{
    bool vbus_present;
    int ret = npm1300_vbus_present(config, &vbus_present);

    if (ret != 0) {
        LOG_ERR("Cannot verify VBUS before ship mode: %d", ret);
        return ret;
    }

    if (vbus_present || zmk_usb_is_powered()) {
        LOG_INF("VBUS present; deferring low-battery ship mode");
        return -EBUSY;
    }

    npm1300_disable_nonessential_loads(config);
    LOG_ERR("Battery critical; entering nPM1300 ship mode");

    return mfd_npm1300_reg_write(config->pmic, NPM1300_SHPHLD_BASE,
                                 NPM1300_TASK_ENTER_SHIP_OFFSET, 1U);
}

static int npm1300_fuel_gauge_init(struct npm1300_vbat_data *data,
                                   const struct npm1300_measurement *measurement)
{
    const struct nrf_fuel_gauge_init_parameters parameters = {
        .v0 = measurement->voltage,
        .i0 = measurement->current,
        .t0 = (float)CONFIG_ZMK_NPM1300_FUEL_GAUGE_ASSUMED_TEMP_C,
        .model = &battery_model,
        .opt_params = NULL,
    };
    int ret = nrf_fuel_gauge_init(&parameters, NULL);

    if (ret != 0) {
        LOG_ERR("Failed to initialize nRF Fuel Gauge: %d", ret);
        return ret;
    }

    data->last_fuel_gauge_update_ms = k_uptime_get();
    data->fuel_gauge_initialized = true;

    LOG_INF("nRF Fuel Gauge %s initialized with LP602760 model",
            nrf_fuel_gauge_version);
    return 0;
}

static void npm1300_fuel_gauge_update(struct npm1300_vbat_data *data,
                                      const struct npm1300_measurement *measurement,
                                      int64_t now_ms)
{
    if (!data->fuel_gauge_initialized) {
        if (npm1300_fuel_gauge_init(data, measurement) != 0) {
            return;
        }
    }

    float delta_seconds = (float)(now_ms - data->last_fuel_gauge_update_ms) / 1000.0f;
    if (delta_seconds <= 0.0f) {
        delta_seconds = 0.001f;
    }

    float soc = nrf_fuel_gauge_process(
        measurement->voltage, measurement->current,
        (float)CONFIG_ZMK_NPM1300_FUEL_GAUGE_ASSUMED_TEMP_C, delta_seconds, NULL);

    data->last_fuel_gauge_update_ms = now_ms;

    if (!isfinite(soc)) {
        LOG_ERR("Fuel gauge returned invalid state of charge");
        return;
    }

    if (soc < 0.0f) {
        soc = 0.0f;
    } else if (soc > 100.0f) {
        soc = 100.0f;
    }

    atomic_set(&data->state_of_charge, (atomic_val_t)(soc + 0.5f));
    atomic_set(&data->fuel_gauge_valid, 1);
    LOG_DBG("Battery: %d mV, SOC %u%%", measurement->millivolts,
            (uint8_t)atomic_get(&data->state_of_charge));
}

static bool npm1300_run_voltage_protection(struct npm1300_vbat_data *data,
                                           const struct npm1300_measurement *measurement,
                                           int64_t now_ms)
{
    /* A stale first ADC result can be zero. Never turn rails off or enter ship
     * mode unless the reading is physically plausible for a connected LiPo. */
    if (!npm1300_battery_voltage_is_plausible(measurement->millivolts)) {
        LOG_WRN("Ignoring implausible battery reading: %d mV", measurement->millivolts);
        return true;
    }

    int64_t interval_ms =
        (int64_t)CONFIG_ZMK_NPM1300_MONITOR_INTERVAL_SECONDS * 1000;

    if (data->last_protection_update_ms != 0 &&
        now_ms - data->last_protection_update_ms < interval_ms) {
        return true;
    }

    data->last_protection_update_ms = now_ms;

    const struct npm1300_vbat_config *config = data->dev->config;
    const struct npm1300_battery_thresholds thresholds = {
        .warning_mv = CONFIG_ZMK_NPM1300_WARNING_MV,
        .ship_mv = CONFIG_ZMK_NPM1300_SHIP_MV,
        .recovery_mv = CONFIG_ZMK_NPM1300_RECOVERY_MV,
        .warning_confirm_samples = CONFIG_ZMK_NPM1300_WARNING_CONFIRM_SAMPLES,
        .ship_confirm_samples = CONFIG_ZMK_NPM1300_SHIP_CONFIRM_SAMPLES,
    };
    enum npm1300_battery_action action =
        npm1300_battery_step(&data->battery_state, &thresholds,
                             measurement->millivolts, measurement->vbus_present);

    switch (action) {
    case NPM1300_BATTERY_ACTION_DISABLE_LOAD:
        LOG_WRN("Low battery: %d mV; disabling nonessential loads",
                measurement->millivolts);
        npm1300_disable_nonessential_loads(config);
        break;
    case NPM1300_BATTERY_ACTION_ENABLE_LOAD:
        /* The RGB driver owns rail enablement; recovery only removes the veto. */
        LOG_INF("Battery recovered: %d mV; nonessential loads may resume",
                measurement->millivolts);
        break;
    case NPM1300_BATTERY_ACTION_ENTER_SHIP:
        if (npm1300_enter_ship_mode(config) == 0) {
            return false;
        }
        break;
    default:
        /* Do not fight the RGB state machine by repeatedly switching its rail
         * off. Load shedding happens once when the warning threshold crosses. */
        break;
    }

    return true;
}

static uint32_t npm1300_next_interval_ms(bool vbus_present)
{
    if (vbus_present) {
        return CONFIG_ZMK_NPM1300_FUEL_GAUGE_CHARGING_INTERVAL_MS;
    }

    if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
        return CONFIG_ZMK_NPM1300_FUEL_GAUGE_ACTIVE_INTERVAL_MS;
    }

    return CONFIG_ZMK_NPM1300_FUEL_GAUGE_IDLE_INTERVAL_MS;
}

static void npm1300_monitor_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    struct npm1300_vbat_data *data = arg1;
    const struct npm1300_vbat_config *config = data->dev->config;

    while (true) {
        struct npm1300_measurement measurement;
        int ret = npm1300_read_measurement(config, &measurement);
        uint32_t next_interval_ms;

        if (ret == 0) {
            int64_t now_ms = k_uptime_get();

            if (!npm1300_battery_voltage_is_plausible(measurement.millivolts)) {
                LOG_WRN("Ignoring implausible battery reading: %d mV",
                        measurement.millivolts);
                next_interval_ms = npm1300_next_interval_ms(measurement.vbus_present);
                k_msleep(next_interval_ms);
                continue;
            }

            npm1300_update_filtered_voltage(data, measurement.millivolts);
            npm1300_fuel_gauge_update(data, &measurement, now_ms);
            if (!npm1300_run_voltage_protection(data, &measurement, now_ms)) {
                return;
            }

            next_interval_ms = npm1300_next_interval_ms(measurement.vbus_present);
        } else {
            LOG_ERR("Failed to monitor battery: %d", ret);
            next_interval_ms = npm1300_next_interval_ms(zmk_usb_is_powered());
        }

        k_msleep(next_interval_ms);
    }
}

static int npm1300_vbat_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    const struct npm1300_vbat_config *config = dev->config;
    struct npm1300_vbat_data *data = dev->data;

    if (chan == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        /* The gauge is intentionally only run by its dedicated thread. Battery
         * reporting will retry after the first asynchronous estimate is ready. */
        return atomic_get(&data->fuel_gauge_valid) ? 0 : -EAGAIN;
    }

    if (chan == SENSOR_CHAN_NPM1300_CHARGER_STATUS) {
        return sensor_sample_fetch_chan(config->charger, chan);
    }

    if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_VOLTAGE &&
        chan != SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }

    struct npm1300_measurement measurement;
    int ret = npm1300_read_measurement(config, &measurement);

    if (ret == 0) {
        npm1300_update_filtered_voltage(data, measurement.millivolts);
    }

    return ret;
}

static int npm1300_vbat_channel_get(const struct device *dev, enum sensor_channel chan,
                                    struct sensor_value *value)
{
    const struct npm1300_vbat_config *config = dev->config;
    struct npm1300_vbat_data *data = dev->data;

    if (chan == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        if (!atomic_get(&data->fuel_gauge_valid)) {
            return -EAGAIN;
        }

        value->val1 = atomic_get(&data->state_of_charge);
        value->val2 = 0;
        return 0;
    }

    if (chan == SENSOR_CHAN_NPM1300_CHARGER_STATUS) {
        return sensor_channel_get(config->charger, chan, value);
    }

    if (chan != SENSOR_CHAN_VOLTAGE && chan != SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }

    if (!data->filtered_voltage_valid) {
        return -EAGAIN;
    }

    value->val1 = data->filtered_mv / 1000;
    value->val2 = (data->filtered_mv % 1000) * 1000;
    return 0;
}

static int npm1300_vbat_init(const struct device *dev)
{
    const struct npm1300_vbat_config *config = dev->config;
    struct npm1300_vbat_data *data = dev->data;
    struct sensor_value enable = {.val1 = 1};

    if (!device_is_ready(config->charger) || !device_is_ready(config->pmic) ||
        !device_is_ready(config->ext_power)) {
        return -ENODEV;
    }

    int ret = sensor_attr_set(config->charger,
                              SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT,
                              SENSOR_ATTR_CONFIGURATION, &enable);
    if (ret != 0) {
        LOG_ERR("Failed to clear charger errors and enable charging: %d", ret);
        return ret;
    }

    data->dev = dev;
    k_tid_t monitor_tid = k_thread_create(
        &data->monitor_thread, data->monitor_stack,
        K_KERNEL_STACK_SIZEOF(data->monitor_stack), npm1300_monitor_thread,
        data, NULL, NULL, K_PRIO_PREEMPT(10), NPM1300_MONITOR_THREAD_OPTIONS,
        K_SECONDS(2));
    k_thread_name_set(monitor_tid, "npm1300_vbat");
    return 0;
}

static DEVICE_API(sensor, npm1300_vbat_api) = {
    .sample_fetch = npm1300_vbat_sample_fetch,
    .channel_get = npm1300_vbat_channel_get,
};

#define NPM1300_VBAT_INIT(inst)                                                                   \
    BUILD_ASSERT(CONFIG_ZMK_NPM1300_SHIP_MV < CONFIG_ZMK_NPM1300_WARNING_MV,                     \
                 "Ship threshold must be below the load-shed threshold");                        \
    BUILD_ASSERT(CONFIG_ZMK_NPM1300_RECOVERY_MV > CONFIG_ZMK_NPM1300_WARNING_MV,                 \
                 "Recovery threshold must be above the load-shed threshold");                   \
    static struct npm1300_vbat_data npm1300_vbat_data_##inst;                                    \
    static const struct npm1300_vbat_config npm1300_vbat_config_##inst = {                       \
        .charger = DEVICE_DT_GET(DT_INST_PHANDLE(inst, charger)),                                \
        .pmic = DEVICE_DT_GET(DT_INST_PHANDLE(inst, pmic)),                                      \
        .ext_power = DEVICE_DT_GET(DT_INST_PHANDLE(inst, ext_power)),                            \
    };                                                                                            \
    SENSOR_DEVICE_DT_INST_DEFINE(inst, npm1300_vbat_init, NULL, &npm1300_vbat_data_##inst,       \
                                 &npm1300_vbat_config_##inst, POST_KERNEL,                        \
                                 CONFIG_ZMK_NPM1300_INIT_PRIORITY, &npm1300_vbat_api);

DT_INST_FOREACH_STATUS_OKAY(NPM1300_VBAT_INIT)
