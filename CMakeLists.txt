cmake_minimum_required(VERSION 3.0)

set (CMAKE_CXX_STANDARD 20)

project (render)

if(MSVC)
        set(CMAKE_FIND_LIBRARY_PREFIXES "")
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".lib" ".dll")
        set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
        set(BUILD_SHARED_LIBS TRUE)
        set(WINDOWS_LIBS 
                bcrypt
                secur32
                mfplat
                mfuuid
                strmiids
                Ws2_32
        )
        set(CompilerFlags
                CMAKE_CXX_FLAGS
                CMAKE_CXX_FLAGS_DEBUG
                CMAKE_CXX_FLAGS_RELEASE
                CMAKE_C_FLAGS
                CMAKE_C_FLAGS_DEBUG
                CMAKE_C_FLAGS_RELEASE
        )
        # TODO: debug
        set(CMAKE_C_FLAGS_DEBUG "/fsanitize=address")
        foreach(CompilerFlag ${CompilerFlags})
                string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
        endforeach()
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2")
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2")
else()    
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")
endif()

find_package(OpenMP REQUIRED)

find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
find_library(AVCODEC_LIBRARY avcodec REQUIRED)

find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h REQUIRED)
find_library(AVFORMAT_LIBRARY avformat REQUIRED)

find_library(AVUTIL_LIBRARY avutil REQUIRED)

find_library(SWRESAMPLE_LIBRARY swresample REQUIRED)

find_library(OPENJPEG_LIBRARY openjp2 REQUIRED)

find_library(OPENH264_LIBRARY openh264 REQUIRED)

find_library(X264_LIBRARY x264 REQUIRED)

include_directories(
        ${AVCODEC_INCLUDE_DIR}
        ${AVFORMAT_INCLUDE_DIR}
)

set (sources 
        src/render.cpp
        src/pipeline/segmentation.cpp
        src/pipeline/join.cpp
)

add_library (${PROJECT_NAME} SHARED ${sources})

# set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS_RELEASE -s)

target_link_libraries(${PROJECT_NAME} 
        PRIVATE
        OpenMP::OpenMP_CXX
        ${AVCODEC_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${OPENJPEG_LIBRARY}
        ${OPENH264_LIBRARY}
        ${X264_LIBRARY}
        ${WINDOWS_LIBS}
)