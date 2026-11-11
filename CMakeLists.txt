cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lvgl_demo_v9)

# GCC 15 is stricter than GCC 14 about section attribute conflicts.
# LVGL 9.2 has LV_ATTRIBUTE_FAST_MEM on both declarations and definitions,
# which GCC 15 treats as a hard error. Downgrade to a warning project-wide.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=attributes" APPEND)