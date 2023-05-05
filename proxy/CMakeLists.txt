if(NOT UNIX)
    message(FATAL_ERROR "libusb proxying is only supported on unix")
endif()

include(cmake/wirecall.cmake)
include(cmake/cli11.cmake)
include(cmake/fmt.cmake)

add_library(usb-1.0-proxy SHARED ${common_sources})

set_target_properties(usb-1.0-proxy PROPERTIES PREFIX "lib")
set_target_properties(usb-1.0-proxy PROPERTIES OUTPUT_NAME "usb-1.0")

target_include_directories(usb-1.0-proxy PUBLIC "${libusb_ROOT}")
target_include_directories(usb-1.0-proxy PUBLIC "${libusb_BINARY_ROOT}/gen_include")
target_include_directories(usb-1.0-proxy PRIVATE "${libusb_ROOT}/os")

target_link_libraries(usb-1.0-proxy PRIVATE wirecall wirecall-asio fmt)

target_sources(usb-1.0-proxy PRIVATE
    "${libusb_ROOT}/os/events_posix.c"
    "${libusb_ROOT}/os/threads_posix.c"

    "${libusb_ROOT}/os/events_posix.h"
    "${libusb_ROOT}/os/threads_posix.h"

    "${CMAKE_CURRENT_LIST_DIR}/client.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/proxy.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/client.hpp"
)

add_executable(server
    "${CMAKE_CURRENT_LIST_DIR}/server.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/proxy.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/impl.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/server.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/log.hpp"
)

target_link_libraries(server PRIVATE wirecall wirecall-asio CLI11 fmt usb-1.0)
target_compile_features(server PRIVATE cxx_std_20)
