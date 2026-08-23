cmake_minimum_required(VERSION 3.10)
project(isp_pipeline)

include(CTest)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libisp_tuning
                 ${CMAKE_CURRENT_BINARY_DIR}/libisp_tuning)

# Include paths for kernel headers (V4L2, ISP control structs)
# On the KV260 board these are in /usr/include
set(KERNEL_INCLUDE_DIR "/usr/include" CACHE PATH "Path to kernel userspace headers")

add_executable(isp_pipeline
    main.cpp
)

target_include_directories(isp_pipeline PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${KERNEL_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/../linux   # for xil-isp-lite.h
)
target_link_libraries(isp_pipeline
    infinite_isp_tuning
    Threads::Threads
    ${OpenCV_LIBS}
)
