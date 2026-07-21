cmake_minimum_required(VERSION 3.16)
project(VarjoGazeDot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

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
    VarjoLib
    opengl32
)

add_custom_command(TARGET VarjoGazeDot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VARJO_SDK_DIR}/bin/VarjoLib.dll"
        "$<TARGET_FILE_DIR:VarjoGazeDot>/VarjoLib.dll"
)

find_package(OpenCV REQUIRED)

add_executable(opencvTest opencvTest.cpp 
    webcam_source.cpp)
target_link_libraries(opencvTest PRIVATE ${OpenCV_LIBS})