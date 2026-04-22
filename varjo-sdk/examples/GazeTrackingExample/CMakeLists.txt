# Copyright 2021 Varjo Technologies Oy. All rights reserved.

# Project name
set(_app_name "GazeTrackingExample")
project(${_app_name})

# Runtime output directories
set(_build_output_dir ${CMAKE_BINARY_DIR}/bin)
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${_build_output_dir})
endforeach(OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES)

# Application sources
set(_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(_sources_app
    ${_src_dir}/Application.cpp
    ${_src_dir}/Application.hpp
    ${_src_dir}/Console.hpp
    ${_src_dir}/CsvWriter.hpp
    ${_src_dir}/GazeTracking.cpp
    ${_src_dir}/GazeTracking.hpp
    ${_src_dir}/UI.cpp
    ${_src_dir}/UI.hpp
    ${_src_dir}/main.cpp
)

# Public common sources
set(_src_common_dir ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
set(_sources_common
    ${_src_common_dir}/Session.cpp
    ${_src_common_dir}/Session.hpp
)

# Visual studio source groups
source_group("Application" FILES ${_sources_app})
source_group("Common" FILES ${_sources_common})

# Application exe target
set(_target ${_app_name})
add_executable(${_target}
    ${_sources_app}
    ${_sources_common}
)

# Include directories
target_include_directories(${_target} PRIVATE ${_src_common_dir})

# Linked libraries
target_link_libraries(${_target}
    PRIVATE VarjoLib
    PRIVATE CxxOpts
)

# VS properties
set_target_properties(${_target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_property(TARGET ${_target} PROPERTY FOLDER "Examples")

# Console application
set_target_properties(${_target} PROPERTIES LINK_FLAGS /SUBSYSTEM:CONSOLE)

# Preprocerssor definitions
target_compile_definitions(${_target} PUBLIC -D_UNICODE -DUNICODE)
