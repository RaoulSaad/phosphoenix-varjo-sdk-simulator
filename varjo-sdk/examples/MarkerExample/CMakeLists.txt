set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(_build_output_dir ${CMAKE_BINARY_DIR}/bin)
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${_build_output_dir})
endforeach(OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES)
set(_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(_src_common_dir ${CMAKE_CURRENT_SOURCE_DIR}/../Common)

file(
    GLOB _source_list
    LIST_DIRECTORIES false

    ${_src_dir}/MarkerScene.hpp
    ${_src_dir}/number_atlas_base64.hpp

    ${_src_dir}/main.cpp
    ${_src_dir}/MarkerScene.cpp
)

file(
    GLOB _source_list_common
    LIST_DIRECTORIES false

    ${_src_common_dir}/D3D11MultiLayerView.hpp
    ${_src_common_dir}/D3D11MultiLayerView.cpp
    ${_src_common_dir}/D3D11Renderer.hpp
    ${_src_common_dir}/D3D11Renderer.cpp
    ${_src_common_dir}/D3D11Shaders.hpp
    ${_src_common_dir}/D3D11Shaders.cpp
    ${_src_common_dir}/ExampleShaders.hpp
    ${_src_common_dir}/Globals.hpp
    ${_src_common_dir}/Globals.cpp
    ${_src_common_dir}/MultiLayerView.hpp
    ${_src_common_dir}/MultiLayerView.cpp
    ${_src_common_dir}/Renderer.hpp
    ${_src_common_dir}/Renderer.cpp
    ${_src_common_dir}/Scene.hpp
    ${_src_common_dir}/Scene.cpp
    ${_src_common_dir}/SyncView.hpp
    ${_src_common_dir}/SyncView.cpp
)

source_group("Common" FILES ${_source_list_common})

set(_target MarkerExample)
add_executable(${_target} ${_source_list} ${_source_list_common})

target_include_directories(${_target} PRIVATE ${_src_common_dir})

set_property(TARGET ${_target} PROPERTY FOLDER "Examples")
set_target_properties(${_target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

target_compile_definitions(${_target} PUBLIC -D_UNICODE -DUNICODE -DNOMINMAX)

target_link_libraries(${_target}
    PRIVATE VarjoLib
    PRIVATE GLM
    PRIVATE d3d11
    PRIVATE dxgi
    PRIVATE d3dcompiler
    PRIVATE windowscodecs
)
