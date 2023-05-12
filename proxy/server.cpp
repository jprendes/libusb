/*
* Redirection server that can talk to the libusb redir backend
* Copyright (c) 2023 Amaury Pouly <amaury.pouly@lowrisc.org>
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
#include "log.hpp"
#include "server.hpp"

#include <asio.hpp>
#include <CLI/CLI.hpp>
#include <wirecall.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

asio::awaitable<void> listener(libusb::proxy::endpoint endpoint) {
    try {
        auto executor = co_await asio::this_coro::executor;
        libusb::proxy::acceptor acceptor{executor, endpoint};
        for (;;) {
            auto socket = co_await acceptor.accept();
            co_spawn(executor, libusb::proxy::serve(std::move(socket)), asio::detached);
        }
    } catch (std::exception & ex) {
        libusb::proxy::log::err("[{}] {}", endpoint.to_string(), ex.what());
        throw;
    }
}

asio::awaitable<void> listener(std::vector<std::string> addresses) {
    try {
        auto executor = co_await asio::this_coro::executor;
        std::vector<libusb::proxy::endpoint> endpoints;
        for (auto & address : addresses) {
            auto eps = co_await libusb::proxy::parse_uri(std::move(address));
            for (auto & e : eps) {
                endpoints.push_back(std::move(e));
            }
        }
        std::vector<asio::awaitable<void>> servers;
        for (auto & endpoint : endpoints) {
            libusb::proxy::log::info("[{}] listening", endpoint.to_string());
            servers.push_back(listener(std::move(endpoint)));
        }
        co_await libusb::proxy::wait_all({servers.begin(), servers.end()});
    } catch (std::exception & ex) {
        libusb::proxy::log::err("{}", ex.what());
    }
}

asio::awaitable<void> signal_guard(asio::awaitable<void> guarded) {
    std::array<asio::awaitable<void>, 2> awaitables{
        std::move(guarded),
        []() -> asio::awaitable<void> {
            auto executor = co_await asio::this_coro::executor;
            asio::signal_set signals(executor, SIGINT, SIGTERM);
            co_await signals.async_wait(asio::use_awaitable);
        }()
    };
    co_await libusb::proxy::wait_one(awaitables);
}

int main(int argc, char **argv) {
    CLI::App app{"libusb proxy server"};

    std::vector<std::string> addresses{"tcp://localhost:5678"};
    app.add_option("-l,--listen", addresses, "Bind address for listening");

    CLI11_PARSE(app, argc, argv);

    libusb_init(NULL);

    std::thread{[]{
        while(true) {
            libusb_handle_events(NULL);
        }
    }}.detach();

    asio::thread_pool io_context(1);

    co_spawn(
        io_context,
        signal_guard(listener(std::move(addresses))),
        asio::use_future
    ).get();
    io_context.stop();
    io_context.join();

    return 0;
}