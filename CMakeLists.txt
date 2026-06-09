cmake_minimum_required(VERSION 3.21)

set(LIB_VERSION 1.0.0)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(BUILD_TESTS "Build tests" OFF)
    if (NOT BUILD_TESTS)
        include(FetchContent)
        set(GITHUB_SUBDIR ../)
        FetchContent_Declare(toolchain-gnu
                             GIT_REPOSITORY https://github.com/HosseinSagha/toolchain-gnu.git
                             GIT_TAG origin/main
                             SYSTEM)
        FetchContent_MakeAvailable(toolchain-gnu)
        FetchContent_GetProperties(toolchain-gnu SOURCE_DIR TOOLCHAIN_DIR)

        message(STATUS
                "Building task-scheduler as a standalone project. Set the target architecture and toolchain in CMakeLists.txt if needed.")
        set(MCU_ARCH "cortex-m4") # Set target architecture for standalone builds
        set(CMAKE_TOOLCHAIN_FILE ${TOOLCHAIN_DIR}/${MCU_ARCH}.cmake)
    endif ()
    project(task-scheduler LANGUAGES C VERSION ${LIB_VERSION})
endif ()

add_library(task-scheduler STATIC
            task-scheduler/core.c
            task-scheduler/scheduler.c
            task-scheduler/scheduler.h
           )

set_target_properties(task-scheduler PROPERTIES
                      VERSION ${LIB_VERSION}
                     )

target_compile_features(task-scheduler PUBLIC c_std_23)
target_include_directories(task-scheduler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if (BUILD_TESTS)
        add_subdirectory(test)
    endif ()
endif ()
