
cmake_minimum_required(VERSION 3.6.0)

# Only generate Debug and Release configuration types.
set(CMAKE_CONFIGURATION_TYPES Debug Release)

# Project name.
project(untrunc VERSION 1.1.0)

#set(UNTRUNC_HDRS
#        src/AP_AtomDefinitions.h
#        src/atom.h
#        src/codec.h
#        src/codecstats.h
#        src/file.h
#        src/h264_sps.h
#        src/log.h
#        src/mp4.h
#        src/track.h
#        )
# 查找source目录下的所有*.cpp,*.c源文件,并将文件列表保存到 DIR_LIB_SRCS 变量
file(GLOB_RECURSE SRC_DIR_LIST "src/*.cpp" "src/avc1/*.cpp" "src/hvc1/*.cpp")

#将DIR_LIB_SRCS中文件列表以静态库的方式加入工程
add_library(
untrunc SHARED
${SRC_DIR_LIST}
)

#set(UNTRUNC_SRCS
#        src/atom.cpp
#        src/codec.cpp
#        src/common.cpp
#        src/file.cpp
#        src/main.cpp
#        src/mp4.cpp
#        src/mutual_pattern.cpp
#        src/track.cpp
#        )
#include_directories("src/main/cpp/include")
#
#add_library(untrunc
#        SHARED
#        ${UNTRUNC_SRCS}
#        )

#add_executable(untrunc ${UNTRUNC_SRCS} ${UNTRUNC_HDRS})

set(jnilibs ${CMAKE_SOURCE_DIR}/../jniLibs)

add_library(avutil
        SHARED
        IMPORTED)
set_target_properties(avutil
        PROPERTIES IMPORTED_LOCATION
        ${jnilibs}/${ANDROID_ABI}/libavutil.so)
#
#add_library(swresample
#        SHARED
#        IMPORTED)
#set_target_properties(swresample
#        PROPERTIES IMPORTED_LOCATION
#        ${jnilibs}/${ANDROID_ABI}/libswresample.so)
#
add_library(avcodec
        SHARED
        IMPORTED)
set_target_properties(avcodec
        PROPERTIES IMPORTED_LOCATION
        ${jnilibs}/${ANDROID_ABI}/libavcodec.so)
#
#add_library(avfilter
#        SHARED
#        IMPORTED)
#set_target_properties(avfilter
#        PROPERTIES IMPORTED_LOCATION
#        ${jnilibs}/${ANDROID_ABI}/libavfilter.so)
#
#add_library(swscale
#        SHARED
#        IMPORTED)
#set_target_properties(swscale
#        PROPERTIES IMPORTED_LOCATION
#        ${jnilibs}/${ANDROID_ABI}/libswscale.so)

add_library(avformat
        SHARED
        IMPORTED)
set_target_properties(avformat
        PROPERTIES IMPORTED_LOCATION
        ${jnilibs}/${ANDROID_ABI}/libavformat.so)

#find_path(FFMPEG_INCLUDE_DIR ./include/libavcodec/)
message(WANNING "CMAKE_SOURCE_DIR = ${CMAKE_SOURCE_DIR}")
set(FFMPEG_INCLUDE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/include/)
message(WANNING "FFMPEG_INCLUDE_DIR = ${FFMPEG_INCLUDE_DIR}")

#find_path(FFMPEG_SOURCE_DIR libavcodec/h264dec.h)

#find_library(AVCODEC_LIBRARY avcodec)
#find_library(AVFORMAT_LIBRARY avformat)
#find_library(AVUTIL_LIBRARY avutil)


#target_include_directories(untrunc PUBLIC ${FFMPEG_INCLUDE_DIR} ${FFMPEG_SOURCE_DIR} ${ATOMICS_INCLUDE_DIR})

find_library( # Sets the name of the path variable.
        log-lib

        # Specifies the name of the NDK library that
        # you want CMake to locate.
        log )

target_include_directories(untrunc PUBLIC  ${FFMPEG_INCLUDE_DIR}
        ${ATOMICS_INCLUDE_DIR})

target_link_libraries(untrunc avcodec avformat avutil ${log-lib})
