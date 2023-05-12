/*
 * Copyright © 2013 Amaury Pouly <amaury.pouly@lowrisc.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "acceptor.hpp"
#include "client.hpp"

#include "libusb.h"
#include "libusbi.h"

#include <errno.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <string>
#include <utility>

#include <asio.hpp>

#include <wirecall.hpp>

namespace {

template <typename T, typename parent_t, void * (getter_fcn)(parent_t *)>
struct priv_ptr {
    template <typename... Args>
    static T & init(parent_t * ctx, Args&&... args) {
        return *new (&from(ctx)) T(std::forward<Args>(args)...);
    }

    static T & from(parent_t * ctx) {
        return *static_cast<T *>(getter_fcn(ctx));
    }

    static T take(parent_t * ctx) {
        T value = std::move(from(ctx));
        from(ctx).~T();
        return value;
    }

    priv_ptr() = default;
    priv_ptr(priv_ptr &) = delete;
    priv_ptr(priv_ptr const &) = delete;
    priv_ptr(priv_ptr &&) = default;
};

struct proxy_context_priv : public priv_ptr<proxy_context_priv, libusb_context, usbi_get_context_priv> {
    std::shared_ptr<asio::thread_pool> io_context;
    std::shared_ptr<libusb::proxy::client> client;

    libusb::proxy::client * operator->() {
        return client.get();
    }
};

struct proxy_device_priv : public priv_ptr<proxy_device_priv, libusb_device, usbi_get_device_priv> {
    uint32_t id = 0;
};

struct proxy_transfer_priv : public priv_ptr<proxy_transfer_priv, usbi_transfer, usbi_get_transfer_priv> {
    libusb::proxy::proxy::transfer_result result;
};

asio::ip::tcp::endpoint proxy_get_server_endpoint(libusb_context * ctx, auto & io_context) {
    const char * host = std::getenv("LIBUSB_PROXY_HOST");
    const char * port = std::getenv("LIBUSB_PROXY_PORT");

    if (!host) host = "localhost";
    if (!port) port = "5678";

    usbi_err(ctx, "resolving: %s:%s", host, port);

    asio::ip::tcp::resolver resolver{io_context};
    auto resolution = resolver.resolve(
        host ? host : "localhost",
        port ? port : "5678",
        asio::ip::tcp::resolver::numeric_service
    );

    if (resolution.empty()) {
        throw std::runtime_error("can't resolve server hostname");
    }

    return *resolution.begin();
}

std::vector<std::string_view> split(std::string_view str, std::string_view delim) {
    std::vector<std::string_view> tokens;
    size_t prev = 0, pos = 0;
    do {
        pos = str.find(delim, prev);
        if (pos == std::string_view::npos) pos = str.length();
        auto token = str.substr(prev, pos-prev);
        if (!token.empty()) tokens.push_back(token);
        prev = pos + delim.length();
    } while (pos < str.length() && prev < str.length());
    return tokens;
}

asio::awaitable<asio::generic::stream_protocol::socket> get_connected_socket(libusb_context * ctx) {
    const char * host_env = std::getenv("LIBUSB_PROXY_HOST");
    if (!host_env) host_env = "tcp://localhost:5678";
    auto hosts = split(host_env, ";");

    for (auto host : hosts) {
        auto endpoints = co_await libusb::proxy::parse_uri(std::string(host));
        for (size_t i = 0; i < endpoints.size(); ++i) {
            try {
                co_return co_await libusb::proxy::connect(endpoints[i]);
            } catch (std::exception & ex) {
                auto ep = endpoints[i];
                usbi_err(ctx, "failed to connect to endpoint %lu %s: %s", i, ep.to_string().c_str(), ex.what());
                continue;
            }
        }
    }
    throw std::runtime_error("failed to connect every endpoint");
}

asio::awaitable<int> proxy_async_init(libusb_context * ctx) {
    auto & priv = proxy_context_priv::from(ctx);

    try {
        auto socket = co_await get_connected_socket(ctx);
        priv.client = std::make_unique<libusb::proxy::client>(std::move(socket));
        priv->run(asio::detached);
    } catch (std::exception & ex) {
        usbi_err(ctx, "failed to connect to host: %s", ex.what());
        co_return LIBUSB_ERROR_ACCESS;
    } catch (...) {
        co_return LIBUSB_ERROR_ACCESS;
    }

    auto capabilities = priv->get_capabilities();

    if (capabilities.has_hid_access) {
        usbi_backend.caps |= USBI_CAP_HAS_HID_ACCESS;
    }
    if (capabilities.supports_detach_kernel_driver) {
        usbi_backend.caps |= USBI_CAP_SUPPORTS_DETACH_KERNEL_DRIVER;
    }

    co_return LIBUSB_SUCCESS;
}

int proxy_init(libusb_context * ctx) {
    auto & priv = proxy_context_priv::init(ctx);
    priv.io_context = std::make_unique<asio::thread_pool>(2);

    return asio::co_spawn(*priv.io_context, proxy_async_init(ctx), asio::use_future).get();
}

void proxy_exit(libusb_context *ctx) {
    proxy_context_priv::take(ctx);
}

int proxy_handle_host_exception(libusb_context * ctx, std::exception & ex) {
    std::string_view token = "host error: libusb::error::";
    std::string what = ex.what();
    usbi_err(ctx, "Trying to handle exception: %s", ex.what());
    if (what.starts_with(token)) {
        auto err = std::stoi(what.substr(token.length()));
        return err;
    }
    usbi_err(ctx, "Unhandled exception: %s", ex.what());
    return LIBUSB_ERROR_OTHER;
}

int proxy_get_device_list(libusb_context * ctx, discovered_devs **discdevs) {
    auto & priv = proxy_context_priv::from(ctx);

    std::vector<libusb::proxy::proxy::device> list;
    try {
        list = priv->devices_list();
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(ctx, ex);
    }

    for(auto const & device : list) {
        auto session_id = device.id;
        struct libusb_device * dev = usbi_get_device_by_session_id(ctx, device.id);
        
        /* new device? */
        if(dev == NULL) {
            dev = usbi_alloc_device(ctx, device.id);
            if (dev == NULL) {
                usbi_err(ctx, "failed to allocate a new device structure");
                continue;
            }
            
            auto & dev_priv = proxy_device_priv::init(dev);
            dev_priv.id = device.id;
            dev->bus_number = device.bus_number;
            dev->port_number = device.port_number;
            dev->device_address = device.device_address;

            usbi_info(ctx, "new device dev_id=%lx, bus=%u, port=%u, addr=%u",
                (unsigned long)dev_priv.id, (unsigned)dev->bus_number,
                (unsigned)dev->port_number, (unsigned)dev->device_address);

            /* ask device descriptor */
            libusb::proxy::proxy::descriptor desc;
            try {
                desc = priv->device_descriptor(device.id);
            } catch (std::exception & ex) {
                libusb_unref_device(dev);
                return proxy_handle_host_exception(ctx, ex);
            }

            usbi_dbg(ctx, "got device descriptor\n");
            dev->device_descriptor = libusb_device_descriptor{
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
            usbi_localize_device_descriptor(&dev->device_descriptor);

            /* sanitize device */
            if(usbi_sanitize_device(dev) < 0) {
                libusb_unref_device(dev);
                continue;
            }
        }

        *discdevs = discovered_devs_append(*discdevs, dev);
    }

    return LIBUSB_SUCCESS;
}

int proxy_get_active_config_descriptor(libusb_device *dev, void *buf, size_t len) {
    auto & priv = proxy_context_priv::from(dev->ctx);
    auto & dev_priv = proxy_device_priv::from(dev);
    usbi_dbg(dev->ctx, "get config active descriptor for device id %lx", (unsigned long)dev_priv.id);
    
    /* this should really be cached but we don't at the moment */
    std::vector<uint8_t> buffer;
    try {
        buffer = priv->active_config_descriptor(dev_priv.id);
    } catch (std::exception & ex) {
        usbi_err(dev->ctx, "cannot get config descriptor");
        return proxy_handle_host_exception(dev->ctx, ex);
    }

    usbi_dbg(dev->ctx, "got config descriptor\n");

    size_t copy_len = std::min(len, buffer.size());
    memcpy(buf, buffer.data(), copy_len);
    return copy_len;
}

int proxy_get_config_descriptor(libusb_device *dev, uint8_t idx, void *buf, size_t len) {
    auto & priv = proxy_context_priv::from(dev->ctx);
    auto & dev_priv = proxy_device_priv::from(dev);
    usbi_dbg(dev->ctx, "get config descriptor %x for device id %lx", (unsigned)idx, (unsigned long)dev_priv.id);
    
    /* this should really be cached but we don't at the moment */
    std::vector<uint8_t> buffer;
    try {
        buffer = priv->config_descriptor(dev_priv.id, idx);
    } catch (std::exception & ex) {
        usbi_err(dev->ctx, "cannot get config descriptor");
        return proxy_handle_host_exception(dev->ctx, ex);
    }

    usbi_dbg(dev->ctx, "got config descriptor\n");

    size_t copy_len = std::min(len, buffer.size());
    memcpy(buf, buffer.data(), copy_len);
    return copy_len;
}

int proxy_open(libusb_device_handle *handle) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "open device id %lx", (unsigned long)dev_priv.id);

    try {
        priv->open_device(dev_priv.id);
    } catch (std::exception & ex) {
        usbi_err(handle->dev->ctx, "cannot open device");
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }
    return LIBUSB_SUCCESS;
}

void proxy_close(libusb_device_handle *handle) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "close device id %lx", (unsigned long)dev_priv.id);

    try {
        priv->close_device(dev_priv.id);
    } catch (std::exception & ex) {
        usbi_err(handle->dev->ctx, "cannot close device");
    }
}

void proxy_destroy_device(libusb_device *dev) {
    proxy_device_priv::take(dev);
}

int proxy_get_configuration(libusb_device_handle *handle, uint8_t * config) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "get_configuration id %lx", (unsigned long)dev_priv.id);

    try {
        *config = priv->get_configuration(dev_priv.id);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_set_configuration(libusb_device_handle *handle, int config) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "set_configuration id %lx", (unsigned long)dev_priv.id);

    try {
        priv->set_configuration(dev_priv.id, config);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_claim_interface(libusb_device_handle *handle, uint8_t iface) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "claim_interface id %lx, iface %u", (unsigned long)dev_priv.id, iface);

    try {
        priv->claim_interface(dev_priv.id, iface);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_release_interface(libusb_device_handle *handle, uint8_t iface) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "release_interface id %lx, iface %u", (unsigned long)dev_priv.id, iface);

    try {
        priv->release_interface(dev_priv.id, iface);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_set_interface_altsetting(libusb_device_handle *handle, uint8_t iface, uint8_t altsetting) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "release_interface id %lx, iface %u", (unsigned long)dev_priv.id, iface);

    try {
        priv->release_interface(dev_priv.id, iface);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_clear_halt(libusb_device_handle *handle, unsigned char endpoint) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "clear_halt id %lx, endpoint %u", (unsigned long)dev_priv.id, endpoint);

    try {
        priv->clear_halt(dev_priv.id, endpoint);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_reset_device(libusb_device_handle *handle) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "reset_device id %lx", (unsigned long)dev_priv.id);

    try {
        priv->reset_device(dev_priv.id);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_submit_transfer(usbi_transfer *itransfer) {
    libusb_context *ctx = itransfer->dev->ctx;

    auto & priv = proxy_context_priv::from(itransfer->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(itransfer->dev);
    auto & xfer_priv = proxy_transfer_priv::init(itransfer);

    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);

    /* this code only works for control, bulk and interrupt */
    switch (transfer->type) {
        case LIBUSB_TRANSFER_TYPE_CONTROL:
        case LIBUSB_TRANSFER_TYPE_BULK:
        case LIBUSB_TRANSFER_TYPE_INTERRUPT:
            break;
        default:
            usbi_err(transfer->dev_handle->dev->ctx, "transfer type %d not implemented", transfer->type);
            return LIBUSB_ERROR_NOT_SUPPORTED;
    }

    /* debug */
    bool is_in = !!((transfer->endpoint & LIBUSB_ENDPOINT_IN));
    /* for control transfers, we need to look at the setup packet */
    if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        libusb_control_setup *setup = libusb_control_transfer_get_setup(transfer);
        is_in = !!(setup->bmRequestType & LIBUSB_ENDPOINT_IN);
    }

    usbi_dbg(ctx, "submit transfer: endp=%x (EP%d %s), length=%lu",
             transfer->endpoint,
             transfer->endpoint & LIBUSB_ENDPOINT_ADDRESS_MASK,
             is_in ? "IN" : "OUT",
             (unsigned long)transfer->length);

    size_t buffer_size = 0;
    if(!is_in) {
        buffer_size = transfer->length;
    } else if(transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        /* for IN control transfer, we need to at least send the setup */
        buffer_size = LIBUSB_CONTROL_SETUP_SIZE;
    }

    std::vector<uint8_t> buffer(buffer_size);
    memcpy(buffer.data(), transfer->buffer, buffer.size());

    itransfer->transferred = 0;

    auto result_awaitable = priv->submit_transfer(
        dev_priv.id,
        transfer->timeout,
        transfer->length,
        transfer->endpoint | (is_in ? LIBUSB_ENDPOINT_IN : 0),
        transfer->type,
        buffer
    );

    asio::co_spawn(
        priv->get_executor(),
        [
            result_awaitable = std::move(result_awaitable),
            &xfer_priv,
            ctx,
            itransfer
        ]() mutable -> asio::awaitable<void> {
            try {
                xfer_priv.result = co_await std::move(result_awaitable);
            } catch (std::exception & ex) {
                // this is unexpected!
                usbi_err(ctx, "transfer failed unexpectedly: %s", ex.what());
                xfer_priv.result = {
                    .status = LIBUSB_TRANSFER_ERROR,
                    .length = 0,
                    .data = {},
                };
            }
            usbi_signal_transfer_completion(itransfer);
        },
        asio::detached
    );

    return LIBUSB_SUCCESS;
}

int proxy_cancel_transfer(usbi_transfer *itransfer) {
    return LIBUSB_SUCCESS;
}

int proxy_handle_transfer_completion(usbi_transfer *itransfer) {
    libusb_context *ctx = itransfer->dev->ctx;
    auto result = proxy_transfer_priv::take(itransfer).result;

    usbi_dbg(ctx, "handle transfer completion: status=%u", result.status);

    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);

    if (itransfer->state_flags & USBI_TRANSFER_CANCELLING) {
        usbi_handle_transfer_cancellation(itransfer);
    }

    usbi_dbg(ctx, "transfer status: %u", result.status);

    /* copy data back */
    size_t skip = 0;
    if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        skip = LIBUSB_CONTROL_SETUP_SIZE;
    }
    if(result.length + skip > transfer->length) {
        usbi_err(ctx, "got more data back than expected!");
        result.status = LIBUSB_TRANSFER_OVERFLOW;
    } else {
        memcpy(transfer->buffer + skip, result.data.data(), result.length);
        itransfer->transferred = result.length;
    }

    if (result.status == LIBUSB_TRANSFER_STALL) {
        errno = EAGAIN;
    }

    return usbi_handle_transfer_completion(itransfer, (libusb_transfer_status)result.status);
}

void proxy_clear_transfer_priv(usbi_transfer *itransfer) {
    proxy_transfer_priv::take(itransfer);
}

int proxy_kernel_driver_active(libusb_device_handle * handle, uint8_t iface) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "kernel_driver_active id %lx", (unsigned long)dev_priv.id);

    try {
        if (priv->kernel_driver_active(dev_priv.id, iface)) {
            return 1;
        } else {
            return 0;
        }
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }
}

int proxy_detach_kernel_driver(libusb_device_handle * handle, uint8_t iface) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "detach_kernel_driver id %lx", (unsigned long)dev_priv.id);

    try {
        priv->detach_kernel_driver(dev_priv.id, iface);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

int proxy_attach_kernel_driver(libusb_device_handle * handle, uint8_t iface) {
    auto & priv = proxy_context_priv::from(handle->dev->ctx);
    auto & dev_priv = proxy_device_priv::from(handle->dev);

    usbi_dbg(handle->dev->ctx, "attach_kernel_driver id %lx", (unsigned long)dev_priv.id);

    try {
        priv->attach_kernel_driver(dev_priv.id, iface);
    } catch (std::exception & ex) {
        return proxy_handle_host_exception(handle->dev->ctx, ex);
    }

    return LIBUSB_SUCCESS;
}

}

extern "C"
usbi_os_backend const usbi_backend = {
    .name = "Proxy backend",
    .caps = USBI_CAP_HAS_HID_ACCESS | USBI_CAP_SUPPORTS_DETACH_KERNEL_DRIVER,
    .init = proxy_init,
    .exit = proxy_exit,
    .set_option = nullptr,
    .get_device_list = proxy_get_device_list,
    .hotplug_poll = nullptr,
    .wrap_sys_device = nullptr,
    .open = proxy_open,
    .close = proxy_close,
    .get_active_config_descriptor = proxy_get_active_config_descriptor,
    .get_config_descriptor = proxy_get_config_descriptor,
    .get_config_descriptor_by_value = nullptr,
    .get_configuration = proxy_get_configuration,
    .set_configuration = proxy_set_configuration,
    .claim_interface = proxy_claim_interface,
    .release_interface = proxy_release_interface,
    .set_interface_altsetting = proxy_set_interface_altsetting,
    .clear_halt = proxy_clear_halt,
    .reset_device = proxy_reset_device,
    .alloc_streams = nullptr,
    .free_streams = nullptr,
    .dev_mem_alloc = nullptr,
    .dev_mem_free = nullptr,
    .kernel_driver_active = proxy_kernel_driver_active,
    .detach_kernel_driver = proxy_detach_kernel_driver,
    .attach_kernel_driver = proxy_attach_kernel_driver,
    .destroy_device = proxy_destroy_device,
    .submit_transfer = proxy_submit_transfer,
    .cancel_transfer = proxy_cancel_transfer,
    .clear_transfer_priv = proxy_clear_transfer_priv,
    .handle_events = nullptr,
    .handle_transfer_completion = proxy_handle_transfer_completion,
    .context_priv_size = sizeof(proxy_context_priv),
    .device_priv_size = sizeof(proxy_device_priv),
    .device_handle_priv_size = 0,
    .transfer_priv_size = sizeof(proxy_transfer_priv),
};
