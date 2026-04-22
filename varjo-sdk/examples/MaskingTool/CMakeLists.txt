
set(_app_name "MaskingTool")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(_build_output_dir ${CMAKE_BINARY_DIR}/bin)
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${_build_output_dir})
endforeach(OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES)

# Application sources
set(_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(_sources_app
    ${_src_dir}/main.cpp
    ${_src_dir}/AppLogic.hpp
    ${_src_dir}/AppLogic.cpp
    ${_src_dir}/AppState.hpp
    ${_src_dir}/AppView.hpp
    ${_src_dir}/AppView.cpp
    ${_src_dir}/MaskScene.hpp
    ${_src_dir}/MaskScene.cpp
    ${_src_dir}/Objects.hpp
    ${_src_dir}/Objects.cpp
    ${_src_dir}/Presets.hpp
    ${_src_dir}/Presets.cpp
)

# Application resources
set(_src_resources_dir ${CMAKE_CURRENT_SOURCE_DIR}/res)
set(_sources_resources
    ${_src_resources_dir}/maskingtool-presets.json
)

# Public common sources
set(_src_common_dir ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
set(_sources_common
    ${_src_common_dir}/D3D11MultiLayerView.hpp
    ${_src_common_dir}/D3D11MultiLayerView.cpp
    ${_src_common_dir}/D3D11Renderer.hpp
    ${_src_common_dir}/D3D11Renderer.cpp
    ${_src_common_dir}/D3D11Shaders.hpp
    ${_src_common_dir}/D3D11Shaders.cpp
    ${_src_common_dir}/ExampleShaders.hpp
    ${_src_common_dir}/GfxContext.hpp
    ${_src_common_dir}/GfxContext.cpp
    ${_src_common_dir}/Globals.hpp
    ${_src_common_dir}/Globals.cpp
    ${_src_common_dir}/MarkerTracker.hpp
    ${_src_common_dir}/MarkerTracker.cpp
    ${_src_common_dir}/MultiLayerView.hpp
    ${_src_common_dir}/MultiLayerView.cpp
    ${_src_common_dir}/Renderer.hpp
    ${_src_common_dir}/Renderer.cpp
    ${_src_common_dir}/Scene.hpp
    ${_src_common_dir}/Scene.cpp
    ${_src_common_dir}/SyncView.hpp
    ${_src_common_dir}/SyncView.cpp
    ${_src_common_dir}/UI.hpp
    ${_src_common_dir}/UI.cpp
)

# Visual studio source groups
#source_group("Application" FILES ${_sources_app})
source_group("Resources" FILES ${_sources_resources})
source_group("Common" FILES ${_sources_common})

# Application exe target
set(_target ${_app_name})
add_executable(${_target}
    ${_sources_app}
    ${_sources_resources}
    ${_sources_common}
)

# Include directories
target_include_directories(${_target}
    PRIVATE ${_src_common_dir}
)

# VS debugger properties
set_property(TARGET ${_target} PROPERTY FOLDER "Examples")
set_target_properties(${_target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(${_target} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--console")

# Console / Windows application
if (_build_console_app)
    set_target_properties(${_target} PROPERTIES LINK_FLAGS /SUBSYSTEM:CONSOLE)
else(_build_console_app)
    set_target_properties(${_target} PROPERTIES LINK_FLAGS /SUBSYSTEM:WINDOWS)
endif(_build_console_app)

# Preprocerssor definitions
target_compile_definitions(${_target} PUBLIC -D_UNICODE -DUNICODE -DNOMINMAX ${VarjoLibDefinitions})

# Linked libraries
target_link_libraries(${_target}
    PRIVATE ImGui::ImGui
    PRIVATE ImGui::ImGuiBackends
    PRIVATE FreeType::FreeType
    PRIVATE GLM::GLM
    PRIVATE JSON::JSON
    PRIVATE CxxOpts::CxxOpts
    PRIVATE d3d11
    PRIVATE dxgi
    PRIVATE d3dcompiler
    PRIVATE windowscodecs
    PRIVATE VarjoLib
)

# Copy resource files
add_custom_command(
    TARGET ${_target}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_src_resources_dir}/maskingtool-presets.json ${CMAKE_BINARY_DIR}/bin/
)
