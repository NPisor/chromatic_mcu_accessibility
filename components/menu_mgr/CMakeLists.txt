cmake_minimum_required(VERSION 3.22)

idf_component_register(
    SRCS "menu_mgr.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../osd"
    REQUIRES common dlist lvgl tab
    WHOLE_ARCHIVE)
