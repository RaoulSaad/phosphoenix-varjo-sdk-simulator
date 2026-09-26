cmake_minimum_required(VERSION 3.16)
project(VarjoGazeDot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(phx_shm)

# Point this at your Varjo SDK root
if(NOT DEFINED VARJO_SDK_DIR)
    if(DEFINED ENV{VARJO_SDK_DIR})
        set(VARJO_SDK_DIR "$ENV{VARJO_SDK_DIR}")
    else()
        message(FATAL_ERROR
            "VARJO_SDK_DIR is not set.\n"
            "  cmake -DVARJO_SDK_DIR=\"C:/VarjoSDK\" ..")
    endif()
endif()

message(STATUS "Varjo SDK: ${VARJO_SDK_DIR}")

add_executable(VarjoGazeDot
    main.cpp
    pipeline_types.cpp
    transport.cpp
    varjo_source.cpp
    renderer.cpp)

target_include_directories(VarjoGazeDot PRIVATE
    "${VARJO_SDK_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"   # for glext.h / wglext.h
)

target_link_directories(VarjoGazeDot PRIVATE "${VARJO_SDK_DIR}/lib")

target_link_libraries(VarjoGazeDot PRIVATE
    phx_shm_static
    VarjoLib
    opengl32
)

add_custom_command(TARGET VarjoGazeDot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VARJO_SDK_DIR}/bin/VarjoLib.dll"
        "$<TARGET_FILE_DIR:VarjoGazeDot>/VarjoLib.dll"
)

add_custom_command(TARGET VarjoGazeDot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:phx_shm_shared>"
        "$<TARGET_FILE_DIR:VarjoGazeDot>/"
)

# Webcam mode is optional: it needs OpenCV for capture. Point OpenCV_DIR at
# the prebuilt package's build\ folder to enable it.
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    message(STATUS "OpenCV ${OpenCV_VERSION}: webcam mode enabled")
    target_sources(VarjoGazeDot PRIVATE webcam_source.cpp window_display.cpp)
    target_link_libraries(VarjoGazeDot PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(VarjoGazeDot PRIVATE PHX_HAVE_WEBCAM)
    # Release runtime DLLs next to the exe: opencv_world plus the separate
    # videoio backend DLLs the 5.x package ships. FFmpeg is what opens URLs
    # and video files; without it --webcam <url> fails to open.
    get_target_property(_ocv_dll opencv_world IMPORTED_LOCATION_RELEASE)
    if(_ocv_dll)
        get_filename_component(_ocv_bin "${_ocv_dll}" DIRECTORY)
        file(GLOB _ocv_dlls "${_ocv_bin}/opencv_*.dll")
        list(FILTER _ocv_dlls EXCLUDE REGEX "d\\.dll$")   # skip debug builds
        foreach(_dll ${_ocv_dlls})
            add_custom_command(TARGET VarjoGazeDot POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:VarjoGazeDot>/")
        endforeach()
    endif()
else()
    message(STATUS "OpenCV not found: webcam mode disabled (set -DOpenCV_DIR=<opencv>\\build)")
endif()