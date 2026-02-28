cmake_minimum_required(VERSION 3.21)

project(task-scheduler
    VERSION 1.0.0
    LANGUAGES C
)

# -----------------------------------------------------------------------
# Library target
# -----------------------------------------------------------------------
add_library(scheduler STATIC
    scheduler.c
    scheduler.h
)

# C23 required (constexpr, nullptr, [[nodiscard]], [[maybe_unused]])
target_compile_features(scheduler PUBLIC c_std_23)

# Consumers get the public header by depending on this target
target_include_directories(scheduler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(TARGET platform_headers)
    target_link_libraries(scheduler PUBLIC platform_headers)
endif()
