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

#include "server.hpp"

#include <asio.hpp>
#include <CLI/CLI.hpp>
#include <wirecall.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

asio::awaitable<void> listener(std::string address, uint16_t port) {
    try {
        auto executor = co_await asio::this_coro::executor;

        asio::ip::tcp::resolver resolver{executor};
        auto resolution = co_await resolver.async_resolve(address, std::to_string(port), asio::ip::tcp::resolver::numeric_service | asio::ip::tcp::resolver::passive, asio::use_awaitable);

        if (resolution.empty()) {
            throw std::runtime_error("can't resolve " + address);
        }

        auto endpoint = *resolution.begin();

        asio::ip::tcp::acceptor acceptor(executor, std::move(endpoint));
        for (;;) {
            asio::ip::tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
            co_spawn(executor, libusb::proxy::serve(std::move(socket)), asio::detached);
        }
    } catch (std::exception & ex) {
        std::cerr << ex.what() << "\n";
    }
}

int main(int argc, char **argv) {
    CLI::App app{"libusb proxy server"};

    std::uint16_t port = 5678;
    std::string address = "localhost";

    app.add_option("-p,--port", port, "Port to listen");
    app.add_option("-a,--address", address, "Bind address for listening");

    CLI11_PARSE(app, argc, argv);

    libusb_init(NULL);

    std::thread{[]{
        while(true) {
            libusb_handle_events(NULL);
        }
    }}.detach();

    asio::thread_pool io_context(1);
    co_spawn(io_context, listener(std::move(address), port), asio::use_future).get();
    io_context.stop();
    io_context.join();

    return 0;
}