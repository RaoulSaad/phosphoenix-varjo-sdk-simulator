cmake_minimum_required(VERSION 3.16)
project(phx_shm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    set(PHX_OS_SRC phx_os_win.cpp)
else()
    set(PHX_OS_SRC phx_os_posix.cpp)
endif()

set(PHX_SOURCES phx_shm.cpp ${PHX_OS_SRC})

# Static: linked by the Varjo app.
add_library(phx_shm_static STATIC ${PHX_SOURCES})
target_include_directories(phx_shm_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(phx_shm_static PUBLIC PHX_STATIC)

# Shared: loaded by Python through ctypes.
add_library(phx_shm_shared SHARED ${PHX_SOURCES})
target_include_directories(phx_shm_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(phx_shm_shared PRIVATE PHX_BUILDING_SHARED)
set_target_properties(phx_shm_shared PROPERTIES OUTPUT_NAME phx_shm PREFIX "")
if(UNIX)
    set_target_properties(phx_shm_shared PROPERTIES PREFIX "lib")
    find_package(Threads REQUIRED)
    target_link_libraries(phx_shm_shared PRIVATE Threads::Threads rt)
    target_link_libraries(phx_shm_static PUBLIC Threads::Threads rt)
endif()

enable_testing()

add_executable(phx_layout_test tests/phx_layout_test.cpp)
target_include_directories(phx_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME phx_layout_test COMMAND phx_layout_test)

add_executable(phx_os_test tests/phx_os_test.cpp ${PHX_OS_SRC})
target_include_directories(phx_os_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(UNIX)
    target_link_libraries(phx_os_test PRIVATE Threads::Threads rt)
endif()
add_test(NAME phx_os_test COMMAND phx_os_test)

add_executable(phx_shm_test tests/phx_shm_test.cpp)
target_link_libraries(phx_shm_test PRIVATE phx_shm_static)
add_test(NAME phx_shm_unit COMMAND phx_shm_test)
add_test(NAME phx_shm_torture_threads COMMAND phx_shm_test --threads --frames 20000 --payload 65536)
add_test(NAME phx_shm_torture_threads_big COMMAND phx_shm_test --threads --seconds 5)
