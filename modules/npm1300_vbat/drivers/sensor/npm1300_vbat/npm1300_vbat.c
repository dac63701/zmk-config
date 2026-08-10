/*
 * Copyright (c) 2025 Oleksandr Maslov
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_npm1300_vbat

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mfd/npm1300.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/ext_power.h>
#include <zmk/usb.h>

#include "battery_thresholds.h"

LOG_MODULE_REGISTER(npm1300_vbat, CONFIG_SENSOR_LOG_LEVEL);

/* Nordic nPMx TASKENTERSHIPMODE: SHPHLD base 0x0b, offset 0x02. */
#define NPM1300_SHPHLD_BASE             0x0bU
#define NPM1300_TASK_ENTER_SHIP_OFFSET  0x02U

struct npm1300_vbat_config {
    const struct device *charger;
    const struct device *pmic;
    const struct device *ext_power;
};

struct npm1300_vbat_data {
    const struct device *dev;
    struct k_work_delayable monitor_work;
    struct npm1300_battery_state battery_state;
    int32_t filtered_mv;
    bool filtered_voltage_valid;
};

static void npm1300_update_filtered_voltage(struct npm1300_vbat_data *data, int32_t millivolts)
{
    if (!data->filtered_voltage_valid) {
        data->filtered_mv = millivolts;
        data->filtered_voltage_valid = true;
        return;
    }

    /* Five-second EMA: reject load/charge steps without hiding the long-term trend. */
    data->filtered_mv = (data->filtered_mv * 3 + millivolts + 2) / 4;
}

static int npm1300_vbat_read(const struct device *dev, struct sensor_value *voltage)
{
    const struct npm1300_vbat_config *config = dev->config;
    int ret = sensor_sample_fetch_chan(config->charger, SENSOR_CHAN_GAUGE_VOLTAGE);

    if (ret != 0) {
        return ret;
    }

    return sensor_channel_get(config->charger, SENSOR_CHAN_GAUGE_VOLTAGE, voltage);
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

    if (vbus_present) {
        LOG_INF("VBUS present; deferring low-battery ship mode");
        return -EBUSY;
    }

    npm1300_disable_nonessential_loads(config);
    LOG_ERR("Battery critical; entering nPM1300 ship mode");

    return mfd_npm1300_reg_write(config->pmic, NPM1300_SHPHLD_BASE,
                                 NPM1300_TASK_ENTER_SHIP_OFFSET, 1U);
}

static void npm1300_monitor_work(struct k_work *work)
{
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct npm1300_vbat_data *data =
        CONTAINER_OF(delayable, struct npm1300_vbat_data, monitor_work);
    const struct npm1300_vbat_config *config = data->dev->config;
    struct sensor_value voltage;
    int ret = npm1300_vbat_read(data->dev, &voltage);

    if (ret == 0) {
        int32_t millivolts = voltage.val1 * 1000 + voltage.val2 / 1000;

        struct npm1300_battery_thresholds thresholds = {
            .warning_mv = CONFIG_ZMK_NPM1300_WARNING_MV,
            .ship_mv = CONFIG_ZMK_NPM1300_SHIP_MV,
            .recovery_mv = CONFIG_ZMK_NPM1300_RECOVERY_MV,
            .warning_confirm_samples = CONFIG_ZMK_NPM1300_WARNING_CONFIRM_SAMPLES,
            .ship_confirm_samples = CONFIG_ZMK_NPM1300_SHIP_CONFIRM_SAMPLES,
        };
        bool vbus_present = zmk_usb_is_powered();
        bool pmic_vbus_present = false;
        ret = npm1300_vbus_present(config, &pmic_vbus_present);
        if (ret != 0) {
            LOG_ERR("Failed to read VBUS status: %d", ret);
        } else {
            vbus_present = vbus_present || pmic_vbus_present;
        }

        npm1300_update_filtered_voltage(data, millivolts);
        enum npm1300_battery_action action = npm1300_battery_step(
                &data->battery_state, &thresholds, millivolts, vbus_present);

        switch (action) {
            case NPM1300_BATTERY_ACTION_DISABLE_LOAD:
                LOG_WRN("Low battery: %d mV; disabling nonessential loads", millivolts);
                /* Continue enforcing the cutoff on every monitor cycle. */
                npm1300_disable_nonessential_loads(config);
                break;
            case NPM1300_BATTERY_ACTION_ENABLE_LOAD:
                /* The RGB driver owns rail enablement; recovery only removes the veto. */
                LOG_INF("Battery recovered: %d mV; nonessential loads may resume", millivolts);
                break;
            case NPM1300_BATTERY_ACTION_ENTER_SHIP:
                ret = npm1300_enter_ship_mode(config);
                if (ret == 0) {
                    return;
                }
                break;
            default:
                if (data->battery_state.warning_active) {
                    npm1300_disable_nonessential_loads(config);
                }
                break;
        }
    } else {
        LOG_ERR("Failed to monitor battery voltage: %d", ret);
    }

    k_work_reschedule(&data->monitor_work,
                      K_SECONDS(CONFIG_ZMK_NPM1300_MONITOR_INTERVAL_SECONDS));
}

static int npm1300_vbat_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    const struct npm1300_vbat_config *config = dev->config;
    if (chan == SENSOR_CHAN_NPM1300_CHARGER_STATUS) {
        return sensor_sample_fetch_chan(config->charger, chan);
    }

    if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_VOLTAGE &&
        chan != SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }

    struct sensor_value voltage;
    int ret = npm1300_vbat_read(dev, &voltage);
    if (ret == 0) {
        struct npm1300_vbat_data *data = dev->data;
        int32_t millivolts = voltage.val1 * 1000 + voltage.val2 / 1000;
        npm1300_update_filtered_voltage(data, millivolts);
    }
    return ret;
}

static int npm1300_vbat_channel_get(const struct device *dev, enum sensor_channel chan,
                                    struct sensor_value *value)
{
    const struct npm1300_vbat_config *config = dev->config;
    if (chan == SENSOR_CHAN_NPM1300_CHARGER_STATUS) {
        return sensor_channel_get(config->charger, chan, value);
    }

    if (chan != SENSOR_CHAN_VOLTAGE && chan != SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }

    struct npm1300_vbat_data *data = dev->data;
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

    int ret = sensor_attr_set(config->charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT,
                              SENSOR_ATTR_CONFIGURATION, &enable);
    if (ret != 0) {
        LOG_ERR("Failed to clear charger errors and enable charging: %d", ret);
        return ret;
    }

    data->dev = dev;
    k_work_init_delayable(&data->monitor_work, npm1300_monitor_work);
    /* Let the first ADC conversion and ZMK settings load complete. */
    k_work_schedule(&data->monitor_work, K_SECONDS(5));
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
