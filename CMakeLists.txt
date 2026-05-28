cmake_minimum_required(VERSION 3.21)

set(LIB_VERSION 1.0.0)

add_library(task-scheduler STATIC
        task-scheduler/core.c
        task-scheduler/scheduler.c
        task-scheduler/scheduler.h
)

set_target_properties(${LIB_ID} PROPERTIES
        VERSION ${LIB_VERSION}
        SOVERSION 1
)

target_compile_features(task-scheduler PUBLIC c_std_23)
target_include_directories(task-scheduler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
