cmake_minimum_required(VERSION 3.21)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(STATUS "Building task-scheduler test project.")
    project(task-scheduler-test LANGUAGES C)
    add_subdirectory(.. lib)
endif ()

add_executable(task-scheduler-test main.c)
target_link_libraries(task-scheduler-test PRIVATE task-scheduler)
