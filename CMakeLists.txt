cmake_minimum_required(VERSION 3.10)
project(c270_driver C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g")

find_package(PkgConfig REQUIRED)
# V4L2 headers are part of kernel headers — no pkg_check needed
# libusb removed — using V4L2 instead
pkg_check_modules(LIBJPEG REQUIRED libjpeg)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(GST REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_RTSP REQUIRED gstreamer-rtsp-server-1.0)

include_directories(
    include
    ${LIBJPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_RTSP_INCLUDE_DIRS}
)

# ── Phase C: V4L2 sources (replaces libusb c270_usb/uvc/capture) ──
set(DRIVER_SOURCES
    src/c270_v4l2.c
    src/c270_display.c
    src/c270_stream.c
)

# Main application
add_executable(c270_app
    ${DRIVER_SOURCES}
    src/main.c
)
target_link_libraries(c270_app
    ${LIBJPEG_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_RTSP_LIBRARIES}
    pthread
)
