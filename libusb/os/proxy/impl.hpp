#pragma once

#include "asio/this_coro.hpp"
#include "libusb.h"
#include "libusbi.h"

#include "proxy.hpp"
#include "log.hpp"
#include "wirecall/async_channel.hpp"

#include <sstream>
#include <wirecall.hpp>

#include <cstdint>
#include <stdexcept>
#include <map>
#include <vector>

namespace libusb::proxy {

struct impl : public proxy {
  private:
    struct local_device {
        uint32_t id;

        libusb_device * device; /* this keeps a reference to the device */
        /* number of times the devices has been opened */
        size_t open_count;
        /* we only keep one handle per device, which can be NULL if not opened */
        libusb_device_handle * handle;

        local_device(uint32_t id, libusb_device * device)
          : id(id)
          , device(device)
          , open_count(0)
          , handle(nullptr)
        {
            log::info("new device dev_id={}, bus={}, port={}, addr={}",
                id, libusb_get_bus_number(device), libusb_get_port_number(device),
                libusb_get_device_address(device)
            );
        }
    };

    inline static std::map<uint32_t, local_device> local_devices;
    inline static uint32_t next_device_id = 42;

    /* return whether device exists or not */
    static auto & get_local_device(libusb_device * device) {
        for (auto & [id, dev] : local_devices) {
            if (dev.device == device) return local_devices.at(id);
        }
        auto id = next_device_id++;
        local_devices.insert({id, local_device{id, device}});
        return local_devices.at(id);
    }

    static auto & get_local_device(uint32_t id) {
        if (!local_devices.contains(id)) {
            log::err("device id {} does not exist", id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }
        return local_devices.at(id);
    }

  public:
    auto devices_list() -> std::vector<device> override {
        libusb_device **devs;
        ssize_t count = libusb_get_device_list(NULL, &devs);

        if(count < 0) {
            // Should this be an error instead?
            count = 0;
        }

        std::vector<device> devices;
        devices.resize(count);

        for(int i = 0; i < count; i++) {
            auto & dev = get_local_device(devs[i]);
            devices[i] = {
                .id = dev.id,
                .bus_number = libusb_get_bus_number(devs[i]),
                .port_number = libusb_get_port_number(devs[i]),
                .device_address = libusb_get_device_address(devs[i]),
            };
        }

        /* don't unref, references are held by the global list */
        libusb_free_device_list(devs, 0);

        return devices;
    }

    auto device_descriptor(uint32_t device_id) -> descriptor override {
        auto & device = get_local_device(device_id);

        struct libusb_device_descriptor desc;
        int err = libusb_get_device_descriptor(device.device, &desc);
        if(err != LIBUSB_SUCCESS) {
            log::err("cannot get device descriptor for device id {}", device_id);
            throw libusb_error{err};
        }

        return {
            .bLength = desc.bLength,
            .bDescriptorType = desc.bDescriptorType,
            .bcdUSB = desc.bcdUSB,
            .bDeviceClass = desc.bDeviceClass,
            .bDeviceSubClass = desc.bDeviceSubClass,
            .bDeviceProtocol = desc.bDeviceProtocol,
            .bMaxPacketSize0 = desc.bMaxPacketSize0,
            .idVendor = desc.idVendor,
            .idProduct = desc.idProduct,
            .bcdDevice = desc.bcdDevice,
            .iManufacturer = desc.iManufacturer,
            .iProduct = desc.iProduct,
            .iSerialNumber = desc.iSerialNumber,
            .bNumConfigurations = desc.bNumConfigurations,
        };
    }

    auto raw_config_descriptor(libusb_device * device, libusb_config_descriptor * desc, uint8_t config_index) -> std::vector<uint8_t> {    
        int desc_tot_len = desc->wTotalLength;

        libusb_device_handle *handle;
        auto err = libusb_open(device, &handle);
        if(err < 0) {
            return {};
        }
        
        std::vector<uint8_t> raw(desc_tot_len);

        err = libusb_get_descriptor(handle, LIBUSB_DT_CONFIG, config_index, (uint8_t *)raw.data(), desc_tot_len);
        if(err < 0) {
            libusb_close(handle);
            return {};
        }

        return raw;
    }

    auto active_config_descriptor(uint32_t device_id) -> config override {
        auto & dev = get_local_device(device_id);

        libusb_config_descriptor * desc;
        int err = libusb_get_active_config_descriptor(dev.device, &desc);
        if(err != LIBUSB_SUCCESS) {
            log::err("cannot get active config descriptor for device id {}", device_id);
            throw libusb_error{err};
        }

        auto value = desc->bConfigurationValue;
        libusb_free_config_descriptor(desc);

        auto device_desc = device_descriptor(device_id);

        for (uint8_t i = 0; i < device_desc.bNumConfigurations; ++i) {
            auto config = config_descriptor(device_id, i);
            if (config.bConfigurationValue == value) {
                return config;
            }
        }

        throw libusb_error{LIBUSB_ERROR_NOT_FOUND};
    }

    auto config_descriptor(uint32_t device_id, uint8_t config_index) -> config override {
        auto & dev = get_local_device(device_id);

        libusb_config_descriptor * desc;
        int err = libusb_get_config_descriptor(dev.device, config_index, &desc);
        if(err != LIBUSB_SUCCESS) {
            log::err("cannot get config descriptor {} for device id {}", config_index, device_id);
            throw libusb_error{err};
        }

        auto config = config_descriptor_impl(desc);

        config.raw = raw_config_descriptor(dev.device, desc, config_index);

        libusb_free_config_descriptor(desc);

        return config;
    }

    auto config_descriptor_impl(libusb_config_descriptor const * desc) -> config {
        libusb::proxy::proxy::config config{
            .bConfigurationValue = desc->bConfigurationValue,
            .iConfiguration = desc->iConfiguration,
            .bmAttributes = desc->bmAttributes,
            .bMaxPower = desc->MaxPower,
        };

        for (size_t i = 0; i < desc->bNumInterfaces; ++i) {
            for (size_t j = 0; j < desc->interface[i].num_altsetting; ++j) {
                auto & iface = desc->interface[i].altsetting[j];
                libusb::proxy::proxy::interface interface{
                    .bInterfaceNumber = iface.bInterfaceNumber,
                    .bAlternateSetting = iface.bAlternateSetting,
                    .bInterfaceClass = iface.bInterfaceClass,
                    .bInterfaceSubClass = iface.bInterfaceSubClass,
                    .bInterfaceProtocol = iface.bInterfaceProtocol,
                    .iInterface = iface.iInterface,
                };

                for (size_t k = 0; k < iface.bNumEndpoints; ++k) {
                    auto & ep = iface.endpoint[k];
                    libusb::proxy::proxy::endpoint endpoint{
                        .bEndpointAddress = ep.bEndpointAddress,
                        .bmAttributes = ep.bmAttributes,
                        .wMaxPacketSize = ep.wMaxPacketSize,
                        .bInterval = ep.bInterval,
                        .bRefresh = ep.bRefresh,
                        .bSynchAddress = ep.bSynchAddress,
                    };
                    interface.endpoints.push_back(std::move(endpoint));
                }

                config.interfaces.push_back(std::move(interface));
            }
        }

        return config;
    }

    auto get_configuration(uint32_t device_id) -> uint8_t override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        int config;
        auto err = libusb_get_configuration(dev.handle, &config);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to get configuration for device id {}", device_id);
            throw libusb_error{err};
        }

        return config;
    }

    auto set_configuration(uint32_t device_id, int config) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_set_configuration(dev.handle, config);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to set configuration for device id {}", device_id);
            throw libusb_error{err};
        }
    }

    auto kernel_driver_active(uint32_t device_id, uint8_t iface) -> bool override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_kernel_driver_active(dev.handle, iface);
        if (err != 0 && err != 1) {
            log::err("failed to claim interface {} for device id {}", iface, device_id);
            throw libusb_error{err};
        }

        return !!err;
    }

    auto detach_kernel_driver(uint32_t device_id, uint8_t iface) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_detach_kernel_driver(dev.handle, iface);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to claim interface {} for device id {}", iface, device_id);
            throw libusb_error{err};
        }
    }

    auto attach_kernel_driver(uint32_t device_id, uint8_t iface) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_attach_kernel_driver(dev.handle, iface);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to claim interface {} for device id {}", iface, device_id);
            throw libusb_error{err};
        }
    }

    auto claim_interface(uint32_t device_id, uint8_t iface) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_claim_interface(dev.handle, iface);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to claim interface {} for device id {}", iface, device_id);
            throw libusb_error{err};
        }
    }

    auto release_interface(uint32_t device_id, uint8_t iface) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_release_interface(dev.handle, iface);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to claim interface {} for device id {}", iface, device_id);
            throw libusb_error{err};
        }
    }

    auto set_interface_altsetting(uint32_t device_id, uint8_t iface, uint8_t altsetting) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_set_interface_alt_setting(dev.handle, iface, altsetting);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to set interface {} altsetting {} for device id {}", iface, altsetting, device_id);
            throw libusb_error{err};
        }
    }

    auto clear_halt(uint32_t device_id, unsigned char endpoint) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_clear_halt(dev.handle, endpoint);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to clear halt on endpoint {} for device id {}", endpoint, device_id);
            throw libusb_error{err};
        }
    }

    auto reset_device(uint32_t device_id) -> void override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_ERROR_NO_DEVICE};
        }

        auto err = libusb_reset_device(dev.handle);
        if (err != LIBUSB_SUCCESS) {
            log::err("failed to reset device id {}", device_id);
            throw libusb_error{err};
        }
    }

    auto open_device(uint32_t device_id) -> void override {
        auto & dev = get_local_device(device_id);
        if (dev.open_count++ == 0) {
            int err = libusb_open(dev.device, &dev.handle);
            if(err < 0) {
                log::err("cannot open device {}, err {}", device_id, err);
                throw libusb_error{err};
            }
        }
    }

    auto close_device(uint32_t device_id) -> void override {
        auto & dev = get_local_device(device_id);
        if(dev.open_count == 0) {
            log::warn("ignoring close, the device was not open");
            return;
        }
        if(--dev.open_count == 0) {
            libusb_close(dev.handle);
            dev.handle = NULL;
        }
    }

    auto submit_transfer(uint32_t device_id, uint32_t timeout, uint32_t length, uint8_t endpoint, uint8_t type, std::vector<uint8_t> buffer) -> asio::awaitable<transfer_result> override {
        auto & dev = get_local_device(device_id);

        if(dev.open_count == 0) {
            log::err("device id {} has not been opened", device_id);
            throw libusb_error{LIBUSB_TRANSFER_NO_DEVICE};
        }

        bool is_in = !!(endpoint & LIBUSB_ENDPOINT_IN);
        endpoint = endpoint & (~(LIBUSB_ENDPOINT_IN));

        /* check length */
        size_t expected_length = 0;
        if(!is_in) {
            expected_length = length;
        } else if (type == LIBUSB_TRANSFER_TYPE_CONTROL) {
            expected_length = LIBUSB_CONTROL_SETUP_SIZE;
        }

        if (buffer.size() != expected_length) {
            log::dbg("transfer packet has the wrong size");
            throw libusb_error{LIBUSB_TRANSFER_ERROR};
        }

        libusb_transfer * transfer = libusb_alloc_transfer(0);
        if(transfer == NULL) {
            throw libusb_error{LIBUSB_TRANSFER_ERROR};
        }
        transfer->dev_handle = dev.handle;
        transfer->timeout = timeout;
        transfer->endpoint = endpoint;
        transfer->type = type;
        transfer->length = length;
        transfer->buffer = (uint8_t *)malloc(length);

        memcpy(transfer->buffer, buffer.data(), buffer.size());

        auto executor = co_await asio::this_coro::executor;
        wirecall::async_channel<> completed{executor};

        transfer->user_data = &completed;
        transfer->callback = &transfer_cb;

        int err = libusb_submit_transfer(transfer);
        if(err < 0) {
            free(transfer->buffer);
            log::dbg("transfer submission failed");
            throw libusb_error{LIBUSB_TRANSFER_ERROR};
        }

        /* see https://libusb.sourceforge.io/api-1.0/libusb_mtasync.html on the
         * proper way of doing this, code stolen from the sync.c file */
        //wait_for_completion(transfer);
        log::dbg("transfer submitted, waiting for completion");
        co_await completed.async_receive();

        log::dbg("transfer completed with status {}, actual_length={}, type={} ({}) [{}]",
                (int)transfer->status, transfer->actual_length, transfer->type, is_in ? "IN" : "OUT", (void*)transfer);

        std::vector<uint8_t> whole_buffer(transfer->length);
        memcpy(whole_buffer.data(), transfer->buffer, whole_buffer.size());

        std::stringstream buffer_str;
        for (auto c : buffer) {
            char hex[] = "0123456789abcdef";
            buffer_str << hex[(c & 0xf0) >> 4] << hex[c & 0x0f] << " ";
        }
        std::stringstream whole_buffer_str;
        for (auto c : whole_buffer) {
            char hex[] = "0123456789abcdef";
            whole_buffer_str << hex[(c & 0xf0) >> 4] << hex[c & 0x0f] << " ";
        }

        log::dbg("transfer buffer is...       [{}]", buffer_str.str());
        log::dbg("transfer whole buffer is... [{}]", whole_buffer_str.str());

        /*
        if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
            auto status = transfer->status;
            free(transfer->buffer);
            libusb_free_transfer(transfer);
            throw libusb_error{status};
        }
        */

        size_t skip = 0;
        if (type == LIBUSB_TRANSFER_TYPE_CONTROL) {
            skip = LIBUSB_CONTROL_SETUP_SIZE;
        }
        std::vector<uint8_t> data(transfer->actual_length);
        memcpy(data.data(), transfer->buffer + skip, data.size());

        libusb_free_transfer(transfer);
        free(transfer->buffer);

        std::stringstream data_str;
        for (auto c : data) {
            char hex[] = "0123456789abcdef";
            data_str << hex[(c & 0xf0) >> 4] << hex[c & 0x0f] << " ";
        }
        log::dbg("transfer data is...         [{}]", data_str.str());

        co_return transfer_result{
            .status = transfer->status,
            .length = transfer->actual_length,
            .data = std::move(data)
        };
    }

    auto get_capabilities() -> capabilities override {
        return {
            .has_hid_access = !!libusb_has_capability(LIBUSB_CAP_HAS_HID_ACCESS),
            .supports_detach_kernel_driver = !!libusb_has_capability(LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER),
        };
    }

  private:
    static void LIBUSB_CALL transfer_cb(libusb_transfer * transfer) {
        wirecall::async_channel<> * completed = (wirecall::async_channel<> *)transfer->user_data;
        log::dbg("transfer completed...");
        completed->try_send();
    }
};

}
