cmake_minimum_required(VERSION 3.10)
project(c270_driver C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(LIBJPEG REQUIRED libjpeg)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(GST REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_RTSP REQUIRED gstreamer-rtsp-server-1.0)

include_directories(
    include
    ${LIBUSB_INCLUDE_DIRS}
    ${LIBJPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_RTSP_INCLUDE_DIRS}
)

set(DRIVER_SOURCES
    src/c270_usb.c
    src/c270_uvc.c
    src/c270_capture.c
    src/c270_display.c
    src/c270_stream.c
    src/c270_hotplug.c
)

# Main application
add_executable(c270_app
    ${DRIVER_SOURCES}
    src/main.c
)
target_link_libraries(c270_app
    ${LIBUSB_LIBRARIES}
    ${LIBJPEG_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_RTSP_LIBRARIES}
    pthread
)

# Step-by-step test tools
add_executable(test_step1_usb tools/test_step1_usb.c src/c270_usb.c)
target_link_libraries(test_step1_usb ${LIBUSB_LIBRARIES})

add_executable(test_step2_uvc tools/test_step2_uvc.c src/c270_usb.c src/c270_uvc.c)
target_link_libraries(test_step2_uvc ${LIBUSB_LIBRARIES})

add_executable(test_step3_capture
    tools/test_step3_capture.c
    src/c270_usb.c
    src/c270_uvc.c
    src/c270_capture.c
)
target_link_libraries(test_step3_capture ${LIBUSB_LIBRARIES} ${LIBJPEG_LIBRARIES})

add_executable(test_step4_display
    tools/test_step4_display.c
    src/c270_usb.c
    src/c270_uvc.c
    src/c270_capture.c
    src/c270_display.c
)
target_link_libraries(test_step4_display
    ${LIBUSB_LIBRARIES}
    ${LIBJPEG_LIBRARIES}
    ${SDL2_LIBRARIES}
    pthread
)

# Prove MJPEG not available via libusb
add_executable(prove_no_mjpeg tools/prove_no_mjpeg.c src/c270_usb.c src/c270_uvc.c)
target_link_libraries(prove_no_mjpeg ${LIBUSB_LIBRARIES})

# Test MJPEG probe
add_executable(test_mjpeg_probe tools/test_mjpeg_probe.c src/c270_usb.c src/c270_uvc.c)
target_link_libraries(test_mjpeg_probe ${LIBUSB_LIBRARIES})
