#pragma once

#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/use_future.hpp"
#include "libusb.h"
#include "libusbi.h"

#include "proxy.hpp"
#include "log.hpp"

#include <concepts>
#include <utility>
#include <wirecall.hpp>

#include <cstdint>
#include <stdexcept>
#include <map>
#include <vector>

#include <iostream>

namespace libusb::proxy {

struct client : public proxy {
  private:
    wirecall::ipc_endpoint<std::string> m_endpoint;

    template <typename return_type, typename key_type, typename... Args>
    auto call_sync(key_type && key, Args&&... args) -> return_type {
        return asio::co_spawn(
            m_endpoint.get_executor(),
            m_endpoint.call<return_type>(std::forward<key_type>(key), std::forward<Args>(args)...),
            asio::use_future
        ).get();
    }

  public:
    template <typename socket_type>
    client(socket_type socket) : m_endpoint(std::forward<socket_type>(socket)) {}

    auto get_executor() {
        return m_endpoint.get_executor();
    }

    auto run() -> asio::awaitable<void> {
        co_await m_endpoint.run();
    }

    template <typename token_type>
    auto run(token_type && token) {
        return asio::co_spawn(get_executor(), run(), std::forward<token_type>(token));
    }

    auto close() {
        m_endpoint.close();
    }

    auto get_capabilities() -> capabilities override {
        return call_sync<capabilities>("get_capabilities");
    }

    auto devices_list() -> std::vector<device> override {
        return call_sync<std::vector<device>>("devices_list");
    }

    auto device_descriptor(uint32_t device_id) -> descriptor override {
        return call_sync<descriptor>("device_descriptor", device_id);
    }

    auto active_config_descriptor(uint32_t device_id) -> config override {
        return call_sync<config>("active_config_descriptor", device_id);
    }

    auto config_descriptor(uint32_t device_id, uint8_t config_index) -> config override {
        return call_sync<config>("config_descriptor", device_id, config_index);
    }

    auto get_configuration(uint32_t device_id) -> uint8_t override {
        return call_sync<uint8_t>("get_configuration", device_id);
    }

    auto set_configuration(uint32_t device_id, int config) -> void override {
        return call_sync<void>("set_configuration", device_id, config);
    }

    auto claim_interface(uint32_t device_id, uint8_t iface) -> void override {
        return call_sync<void>("claim_interface", device_id, iface);
    }

    auto release_interface(uint32_t device_id, uint8_t iface) -> void override {
        return call_sync<void>("release_interface", device_id, iface);
    }

    auto kernel_driver_active(uint32_t device_id, uint8_t iface) -> bool override {
        return call_sync<bool>("kernel_driver_active", device_id, iface);
    }

    auto detach_kernel_driver(uint32_t device_id, uint8_t iface) -> void override {
        return call_sync<void>("detach_kernel_driver", device_id, iface);
    }

    auto attach_kernel_driver(uint32_t device_id, uint8_t iface) -> void override {
        return call_sync<void>("attach_kernel_driver", device_id, iface);
    }

    auto set_interface_altsetting(uint32_t device_id, uint8_t iface, uint8_t altsetting) -> void override {
        return call_sync<void>("set_interface_altsetting", device_id, iface, altsetting);
    }

    auto clear_halt(uint32_t device_id, unsigned char endpoint) -> void override {
        return call_sync<void>("clear_halt", device_id, endpoint);
    }

    auto reset_device(uint32_t device_id) -> void override {
        return call_sync<void>("reset_device", device_id);
    }

    auto open_device(uint32_t device_id) -> void override {
        return call_sync<void>("open_device", device_id);
    }

    auto close_device(uint32_t device_id) -> void override {
        return call_sync<void>("close_device", device_id);
    }

    auto submit_transfer(uint32_t device_id, uint32_t timeout, uint32_t length, uint8_t endpoint, uint8_t type, std::vector<uint8_t> buffer) -> asio::awaitable<transfer_result> override {
        co_return co_await m_endpoint.call<transfer_result>("submit_transfer", device_id, timeout, length, endpoint, type, std::move(buffer));
    }
};

}
