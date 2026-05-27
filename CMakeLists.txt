cmake_minimum_required(VERSION 3.21)

project(task-scheduler
        VERSION 1.0.0
        LANGUAGES C
)

# -----------------------------------------------------------------------
# Library target
# -----------------------------------------------------------------------
add_library(task-scheduler STATIC
        task-scheduler/core.c
        task-scheduler/scheduler.c
        task-scheduler/scheduler.h
)

target_compile_features(task-scheduler PUBLIC c_std_23)
target_include_directories(task-scheduler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
