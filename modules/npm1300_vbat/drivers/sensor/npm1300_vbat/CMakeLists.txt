# SPDX-License-Identifier: MIT
zephyr_library()
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
zephyr_library_sources(battery_thresholds.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_NPM1300_VBAT npm1300_vbat.c)
