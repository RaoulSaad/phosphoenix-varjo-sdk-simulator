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

add_executable(VarjoGazeDot main.cpp)

target_include_directories(VarjoGazeDot PRIVATE
    "${VARJO_SDK_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"   # for glext.h / wglext.h
)

target_link_directories(VarjoGazeDot PRIVATE "${VARJO_SDK_DIR}/lib")

target_link_libraries(VarjoGazeDot PRIVATE
    VarjoLib
    opengl32
    Ws2_32
)