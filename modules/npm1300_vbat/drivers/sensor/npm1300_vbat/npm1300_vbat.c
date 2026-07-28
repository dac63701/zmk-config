/*
 * Copyright (c) 2025 Oleksandr Maslov
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_npm1300_vbat

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mfd/npm1300.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(npm1300_vbat, CONFIG_SENSOR_LOG_LEVEL);

/* Nordic nPMx TASKENTERSHIPMODE: SHPHLD base 0x0b, offset 0x02. */
#define NPM1300_SHPHLD_BASE             0x0bU
#define NPM1300_TASK_ENTER_SHIP_OFFSET  0x02U

struct npm1300_vbat_config {
    const struct device *charger;
    const struct device *pmic;
    struct gpio_dt_spec load_switch;
};

struct npm1300_vbat_data {
    const struct device *dev;
    struct k_work_delayable monitor_work;
    uint8_t critical_samples;
    bool warning_active;
};

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
    if (config->load_switch.port != NULL && device_is_ready(config->load_switch.port)) {
        int ret = gpio_pin_set_dt(&config->load_switch, 0);

        if (ret != 0) {
            LOG_ERR("Failed to disable nonessential load: %d", ret);
        }
    }
}

static void npm1300_enable_nonessential_loads(const struct npm1300_vbat_config *config)
{
    if (config->load_switch.port != NULL && device_is_ready(config->load_switch.port)) {
        int ret = gpio_pin_set_dt(&config->load_switch, 1);

        if (ret != 0) {
            LOG_ERR("Failed to restore nonessential load: %d", ret);
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

        if (millivolts <= CONFIG_ZMK_NPM1300_WARNING_MV) {
            /* Keep enforcing this in case an activity event tries to restore LEDs. */
            npm1300_disable_nonessential_loads(config);

            if (!data->warning_active) {
                data->warning_active = true;
                LOG_WRN("Low battery: %d mV; disabling nonessential loads", millivolts);
            }
        } else if (millivolts >= CONFIG_ZMK_NPM1300_RECOVERY_MV) {
            if (data->warning_active) {
                npm1300_enable_nonessential_loads(config);
            }
            data->warning_active = false;
            data->critical_samples = 0;
        }

        if (millivolts <= CONFIG_ZMK_NPM1300_SHIP_MV) {
            if (data->critical_samples < CONFIG_ZMK_NPM1300_SHIP_CONFIRM_SAMPLES) {
                data->critical_samples++;
            }
            if (data->critical_samples >= CONFIG_ZMK_NPM1300_SHIP_CONFIRM_SAMPLES) {
                ret = npm1300_enter_ship_mode(config);
                if (ret == 0) {
                    return;
                }
            }
        } else {
            data->critical_samples = 0;
        }
    } else {
        LOG_ERR("Failed to monitor battery voltage: %d", ret);
    }

    k_work_reschedule(&data->monitor_work,
                      K_SECONDS(CONFIG_ZMK_NPM1300_MONITOR_INTERVAL_SECONDS));
}

static int npm1300_vbat_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_VOLTAGE &&
        chan != SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }

    struct sensor_value unused;
    return npm1300_vbat_read(dev, &unused);
}

static int npm1300_vbat_channel_get(const struct device *dev, enum sensor_channel chan,
                                    struct sensor_value *value)
{
    const struct npm1300_vbat_config *config = dev->config;

    if (chan != SENSOR_CHAN_VOLTAGE && chan != SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }

    return sensor_channel_get(config->charger, SENSOR_CHAN_GAUGE_VOLTAGE, value);
}

static int npm1300_vbat_init(const struct device *dev)
{
    const struct npm1300_vbat_config *config = dev->config;
    struct npm1300_vbat_data *data = dev->data;
    struct sensor_value enable = {.val1 = 1};

    if (!device_is_ready(config->charger) || !device_is_ready(config->pmic)) {
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
        .load_switch = GPIO_DT_SPEC_INST_GET_OR(inst, load_switch_gpios, {0}),                   \
    };                                                                                            \
    SENSOR_DEVICE_DT_INST_DEFINE(inst, npm1300_vbat_init, NULL, &npm1300_vbat_data_##inst,       \
                                 &npm1300_vbat_config_##inst, POST_KERNEL,                        \
                                 CONFIG_ZMK_NPM1300_INIT_PRIORITY, &npm1300_vbat_api);

DT_INST_FOREACH_STATUS_OKAY(NPM1300_VBAT_INIT)
