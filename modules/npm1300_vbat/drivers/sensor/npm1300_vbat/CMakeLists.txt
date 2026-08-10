# SPDX-License-Identifier: MIT
zephyr_library()
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
zephyr_library_sources(battery_thresholds.c)

if(CONFIG_ZMK_NPM1300_VBAT)
  # nrfxlib is fetched by west next to the zmk and zephyr projects. The ZMK
  # build image's Zephyr release does not process nrfxlib's `cmake-ext` module
  # metadata, so make the fuel-gauge API and the matching Cortex-M4 hard-float
  # binary explicit here.
  get_filename_component(NRFXLIB_ROOT
    "${APPLICATION_SOURCE_DIR}/../../nrfxlib" ABSOLUTE)
  set(NRF_FUEL_GAUGE_ROOT "${NRFXLIB_ROOT}/nrf_fuel_gauge")
  set(NRF_FUEL_GAUGE_LIBRARY
    "${NRF_FUEL_GAUGE_ROOT}/lib/cortex-m4/hard-float/libnrf_fuel_gauge.a")

  if(NOT EXISTS "${NRF_FUEL_GAUGE_ROOT}/include/nrf_fuel_gauge.h")
    message(FATAL_ERROR
      "nrf_fuel_gauge.h was not found under ${NRF_FUEL_GAUGE_ROOT}; run west update")
  endif()

  if(NOT EXISTS "${NRF_FUEL_GAUGE_LIBRARY}")
    message(FATAL_ERROR
      "Nordic fuel-gauge library was not found at ${NRF_FUEL_GAUGE_LIBRARY}")
  endif()

  zephyr_library_include_directories("${NRF_FUEL_GAUGE_ROOT}/include")
  zephyr_library_sources(npm1300_vbat.c)
  zephyr_library_link_libraries("${NRF_FUEL_GAUGE_LIBRARY}")
endif()
