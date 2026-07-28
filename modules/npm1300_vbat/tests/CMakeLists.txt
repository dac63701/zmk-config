cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(npm1300_vbat_thresholds)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../drivers/sensor/npm1300_vbat
)
target_sources(app PRIVATE
  src/main.c
  ../drivers/sensor/npm1300_vbat/battery_thresholds.c
)
