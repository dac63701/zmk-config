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

#include <drivers/ext_power.h>
#include <nrf_fuel_gauge.h>
#include <zmk/activity.h>
#include <zmk/usb.h>

#include "battery_thresholds.h"

LOG_MODULE_REGISTER(npm1300_vbat, CONFIG_SENSOR_LOG_LEVEL);

/* Nordic nPMx TASKENTERSHIPMODE: SHPHLD base 0x0b, offset 0x02. */
#define NPM1300_SHPHLD_BASE            0x0bU
#define NPM1300_TASK_ENTER_SHIP_OFFSET 0x02U

/* nPM1300 CHARGER.BCHGCHARGESTATUS bit masks. */
#define NPM1300_CHG_STATUS_COMPLETE BIT(1)
#define NPM1300_CHG_STATUS_TRICKLE  BIT(2)
#define NPM1300_CHG_STATUS_CC       BIT(3)
#define NPM1300_CHG_STATUS_CV       BIT(4)

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
    int32_t charge_status;
    bool vbus_present;
};

struct npm1300_vbat_data {
    const struct device *dev;
    struct k_work_delayable monitor_work;
    struct k_mutex lock;
    struct npm1300_battery_state battery_state;
    int64_t last_fuel_gauge_update_ms;
    int64_t last_protection_update_ms;
    int32_t filtered_mv;
    int32_t last_charge_status;
    uint8_t state_of_charge;
    bool filtered_voltage_valid;
    bool fuel_gauge_initialized;
    bool fuel_gauge_valid;
    bool last_vbus_present;
    bool last_vbus_valid;
};

static float sensor_value_to_float(const struct sensor_value *value)
{
    return (float)value->val1 + ((float)value->val2 / 1000000.0f);
}

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
     * The Zephyr 4.1 nPM1300 driver used by this board reports positive current
     * while discharging and negative current while charging. That is the sign
     * convention expected by the matching nRF Fuel Gauge library.
     */
    measurement->current = sensor_value_to_float(&value);

    ret = sensor_channel_get(config->charger, SENSOR_CHAN_NPM1300_CHARGER_STATUS, &value);
    if (ret != 0) {
        return ret;
    }

    measurement->charge_status = value.val1;

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

static int npm1300_inform_charge_status(int32_t charge_status)
{
    union nrf_fuel_gauge_ext_state_info_data info;

    if ((charge_status & NPM1300_CHG_STATUS_COMPLETE) != 0) {
        info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
    } else if ((charge_status & NPM1300_CHG_STATUS_TRICKLE) != 0) {
        info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
    } else if ((charge_status & NPM1300_CHG_STATUS_CC) != 0) {
        info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
    } else if ((charge_status & NPM1300_CHG_STATUS_CV) != 0) {
        info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
    } else {
        info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
    }

    return nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE,
                                            &info);
}

static int npm1300_inform_vbus(bool present)
{
    return nrf_fuel_gauge_ext_state_update(
        present ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
                : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
        NULL);
}

static int npm1300_fuel_gauge_init(struct npm1300_vbat_data *data,
                                   const struct npm1300_measurement *measurement)
{
    const struct npm1300_vbat_config *config = data->dev->config;
    const struct nrf_fuel_gauge_init_parameters parameters = {
        .v0 = measurement->voltage,
        .i0 = measurement->current,
        .t0 = (float)CONFIG_ZMK_NPM1300_FUEL_GAUGE_ASSUMED_TEMP_C,
        .model = &battery_model,
        .opt_params = NULL,
        .state = NULL,
    };
    int ret = nrf_fuel_gauge_init(&parameters, NULL);

    if (ret != 0) {
        LOG_ERR("Failed to initialize nRF Fuel Gauge: %d", ret);
        return ret;
    }

    struct sensor_value value;
    ret = sensor_channel_get(config->charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT,
                             &value);
    if (ret == 0) {
        float charge_current = sensor_value_to_float(&value);
        union nrf_fuel_gauge_ext_state_info_data info = {
            .charge_current_limit = charge_current,
        };

        ret = nrf_fuel_gauge_ext_state_update(
            NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT, &info);
        if (ret == 0) {
            /* The board's charger termination current is configured as 10 percent. */
            info.charge_term_current = charge_current / 10.0f;
            ret = nrf_fuel_gauge_ext_state_update(
                NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT, &info);
        }
    }

    if (ret != 0) {
        LOG_WRN("Could not provide charger current limits to fuel gauge: %d", ret);
    }

    ret = npm1300_inform_charge_status(measurement->charge_status);
    if (ret != 0) {
        LOG_WRN("Could not provide charger state to fuel gauge: %d", ret);
    }

    ret = npm1300_inform_vbus(measurement->vbus_present);
    if (ret != 0) {
        LOG_WRN("Could not provide VBUS state to fuel gauge: %d", ret);
    }

    data->last_charge_status = measurement->charge_status;
    data->last_vbus_present = measurement->vbus_present;
    data->last_vbus_valid = true;
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

    if (!data->last_vbus_valid || data->last_vbus_present != measurement->vbus_present) {
        int ret = npm1300_inform_vbus(measurement->vbus_present);

        if (ret != 0) {
            LOG_WRN("Could not update fuel-gauge VBUS state: %d", ret);
        }

        data->last_vbus_present = measurement->vbus_present;
        data->last_vbus_valid = true;
    }

    if (data->last_charge_status != measurement->charge_status) {
        int ret = npm1300_inform_charge_status(measurement->charge_status);

        if (ret != 0) {
            LOG_WRN("Could not update fuel-gauge charger state: %d", ret);
        }

        data->last_charge_status = measurement->charge_status;
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

    data->state_of_charge = (uint8_t)(soc + 0.5f);
    data->fuel_gauge_valid = true;
    LOG_DBG("Battery: %d mV, SOC %u%%", measurement->millivolts,
            data->state_of_charge);
}

static bool npm1300_run_voltage_protection(struct npm1300_vbat_data *data,
                                           const struct npm1300_measurement *measurement,
                                           int64_t now_ms)
{
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
        if (data->battery_state.warning_active) {
            npm1300_disable_nonessential_loads(config);
        }
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

static void npm1300_monitor_work(struct k_work *work)
{
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct npm1300_vbat_data *data =
        CONTAINER_OF(delayable, struct npm1300_vbat_data, monitor_work);
    const struct npm1300_vbat_config *config = data->dev->config;
    struct npm1300_measurement measurement;
    k_mutex_lock(&data->lock, K_FOREVER);
    int ret = npm1300_read_measurement(config, &measurement);

    if (ret == 0) {
        int64_t now_ms = k_uptime_get();

        npm1300_update_filtered_voltage(data, measurement.millivolts);
        npm1300_fuel_gauge_update(data, &measurement, now_ms);

        if (!npm1300_run_voltage_protection(data, &measurement, now_ms)) {
            k_mutex_unlock(&data->lock);
            return;
        }

        k_mutex_unlock(&data->lock);
        k_work_reschedule(&data->monitor_work,
                          K_MSEC(npm1300_next_interval_ms(measurement.vbus_present)));
        return;
    }

    k_mutex_unlock(&data->lock);
    LOG_ERR("Failed to monitor battery: %d", ret);
    k_work_reschedule(
        &data->monitor_work,
        K_MSEC(npm1300_next_interval_ms(zmk_usb_is_powered())));
}

static int npm1300_vbat_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    const struct npm1300_vbat_config *config = dev->config;
    struct npm1300_vbat_data *data = dev->data;

    if (chan == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        if (data->fuel_gauge_valid) {
            return 0;
        }

        /* ZMK requests a value immediately at boot, before delayed work may run. */
        struct npm1300_measurement measurement;
        k_mutex_lock(&data->lock, K_FOREVER);
        int ret = npm1300_read_measurement(config, &measurement);

        if (ret == 0) {
            npm1300_update_filtered_voltage(data, measurement.millivolts);
            npm1300_fuel_gauge_update(data, &measurement, k_uptime_get());
            ret = data->fuel_gauge_valid ? 0 : -EAGAIN;
        }

        k_mutex_unlock(&data->lock);
        return ret;
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
        if (!data->fuel_gauge_valid) {
            return -EAGAIN;
        }

        value->val1 = data->state_of_charge;
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
    data->last_charge_status = -1;
    k_mutex_init(&data->lock);
    k_work_init_delayable(&data->monitor_work, npm1300_monitor_work);
    /* Allow the first ADC conversion and ZMK settings initialization to complete. */
    k_work_schedule(&data->monitor_work, K_SECONDS(1));
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
