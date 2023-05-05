#pragma once

#include <asio.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace libusb::proxy {

struct proxy {
    struct capabilities {
        bool has_hid_access;
        bool supports_detach_kernel_driver;
    };

    struct device {
        uint32_t id;
        uint8_t bus_number;
        uint8_t port_number;
        uint8_t device_address;
    };

    struct descriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint16_t bcdUSB;
        uint8_t  bDeviceClass;
        uint8_t  bDeviceSubClass;
        uint8_t  bDeviceProtocol;
        uint8_t  bMaxPacketSize0;
        uint16_t idVendor;
        uint16_t idProduct;
        uint16_t bcdDevice;
        uint8_t  iManufacturer;
        uint8_t  iProduct;
        uint8_t  iSerialNumber;
        uint8_t  bNumConfigurations;
    };

    struct transfer_result {
        int status;
        int32_t length;
        std::vector<uint8_t> data;
    };

    struct libusb_error : public std::runtime_error {
        libusb_error(int err) : std::runtime_error(std::string{"libusb::error::"} + std::to_string(err)) {}
    };

    virtual auto get_capabilities() -> capabilities = 0;
    virtual auto devices_list() -> std::vector<device> = 0;
    virtual auto device_descriptor(uint32_t device_id) -> descriptor = 0;
    virtual auto active_config_descriptor(uint32_t device_id) -> std::vector<uint8_t> = 0;
    virtual auto config_descriptor(uint32_t device_id, uint8_t config_index) -> std::vector<uint8_t> = 0;
    virtual auto get_configuration(uint32_t device_id) -> uint8_t = 0;
    virtual auto set_configuration(uint32_t device_id, int config) -> void = 0;
    virtual auto claim_interface(uint32_t device_id, uint8_t iface) -> void = 0;
    virtual auto release_interface(uint32_t device_id, uint8_t iface) -> void = 0;
    virtual auto kernel_driver_active(uint32_t device_id, uint8_t iface) -> bool = 0;
    virtual auto detach_kernel_driver(uint32_t device_id, uint8_t iface) -> void = 0;
    virtual auto attach_kernel_driver(uint32_t device_id, uint8_t iface) -> void = 0;
    virtual auto set_interface_altsetting(uint32_t device_id, uint8_t iface, uint8_t altsetting) -> void = 0;
    virtual auto clear_halt(uint32_t device_id, unsigned char endpoint) -> void = 0;
    virtual auto reset_device(uint32_t device_id) -> void = 0;
    virtual auto open_device(uint32_t device_id) -> void = 0;
    virtual auto close_device(uint32_t device_id) -> void = 0;
    virtual auto submit_transfer(uint32_t device_id, uint32_t timeout, uint32_t length, uint8_t endpoint, uint8_t type, std::vector<uint8_t> buffer) -> asio::awaitable<transfer_result> = 0;
};

}
