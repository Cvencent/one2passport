cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Internet-Radio VERSION 1.4.3)

# LVGL's software rounded-rect fill allocates a per-row mask and does not check
# lv_malloc() for NULL. On this low-heap C3 target a failed allocation would
# reach lv_memset(NULL, ...) and crash. Keep third-party sources untouched in
# source control, but make the dependency build safe by adding a guard after
# dependency resolution.
set(LVGL_DRAW_FILL_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/lvgl__lvgl/src/draw/sw/lv_draw_sw_fill.c")
if(EXISTS "${LVGL_DRAW_FILL_SOURCE}")
    file(READ "${LVGL_DRAW_FILL_SOURCE}" LVGL_DRAW_FILL_TEXT)
    string(FIND "${LVGL_DRAW_FILL_TEXT}" "LVGL_DRAW_FILL_GUARD" LVGL_DRAW_FILL_GUARD_POS)
    if(LVGL_DRAW_FILL_GUARD_POS EQUAL -1)
        string(REPLACE
            "        mask_buf = lv_malloc(clipped_w);"
            "        mask_buf = lv_malloc(clipped_w);\n        if(mask_buf == NULL) {\n            LV_LOG_WARN(\"LVGL_DRAW_FILL_GUARD: mask allocation failed; deferring rounded fill\");\n            return;\n        }"
            LVGL_DRAW_FILL_TEXT "${LVGL_DRAW_FILL_TEXT}")
        file(WRITE "${LVGL_DRAW_FILL_SOURCE}" "${LVGL_DRAW_FILL_TEXT}")
        message(STATUS "Applied LVGL rounded-fill allocation guard")
    endif()
endif()