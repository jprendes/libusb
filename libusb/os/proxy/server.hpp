#pragma once

#include "impl.hpp"
#include <concepts>
#include <optional>
#include <variant>
#include <wirecall.hpp>
#include <asio.hpp>

#include <cstdint>
#include <vector>

namespace libusb::proxy {

inline asio::awaitable<void> serve(asio::ip::tcp::socket socket) {
    wirecall::ipc_endpoint<std::string> endpoint{std::move(socket)};

    libusb::proxy::impl proxy;

    auto bind = [&proxy]<typename return_type, typename... Args>(return_type (libusb::proxy::impl::*method)(Args...)) {
        return [&proxy, method](Args... args) -> return_type {
            return (proxy.*method)(args...);
        };
    };

    co_await endpoint.add_method("get_capabilities",         bind(&libusb::proxy::impl::get_capabilities));
    co_await endpoint.add_method("devices_list",             bind(&libusb::proxy::impl::devices_list));
    co_await endpoint.add_method("device_descriptor",        bind(&libusb::proxy::impl::device_descriptor));
    co_await endpoint.add_method("active_config_descriptor", bind(&libusb::proxy::impl::active_config_descriptor));
    co_await endpoint.add_method("config_descriptor",        bind(&libusb::proxy::impl::config_descriptor));
    co_await endpoint.add_method("get_configuration",        bind(&libusb::proxy::impl::get_configuration));
    co_await endpoint.add_method("set_configuration",        bind(&libusb::proxy::impl::set_configuration));
    co_await endpoint.add_method("claim_interface",          bind(&libusb::proxy::impl::claim_interface));
    co_await endpoint.add_method("release_interface",        bind(&libusb::proxy::impl::release_interface));
    co_await endpoint.add_method("kernel_driver_active",     bind(&libusb::proxy::impl::kernel_driver_active));
    co_await endpoint.add_method("detach_kernel_driver",     bind(&libusb::proxy::impl::detach_kernel_driver));
    co_await endpoint.add_method("attach_kernel_driver",     bind(&libusb::proxy::impl::attach_kernel_driver));
    co_await endpoint.add_method("set_interface_altsetting", bind(&libusb::proxy::impl::set_interface_altsetting));
    co_await endpoint.add_method("clear_halt",               bind(&libusb::proxy::impl::clear_halt));
    co_await endpoint.add_method("reset_device",             bind(&libusb::proxy::impl::reset_device));
    co_await endpoint.add_method("open_device",              bind(&libusb::proxy::impl::open_device));
    co_await endpoint.add_method("close_device",             bind(&libusb::proxy::impl::close_device));
    co_await endpoint.add_method("submit_transfer",          bind(&libusb::proxy::impl::submit_transfer));

    co_await endpoint.run();
}

}
