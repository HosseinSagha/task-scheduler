cmake_minimum_required(VERSION 3.21)

project(task-scheduler-test
        LANGUAGES C
)

# Pull in the scheduler library from the repo root.
add_subdirectory(.. ..)

add_executable(test main.c)

target_link_libraries(test PRIVATE task-scheduler)
