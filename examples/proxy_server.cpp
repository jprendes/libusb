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

#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <getopt.h>

#include <thread>

#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "libusb.h"
#include "os/proxy/server.hpp"

#include <CLI/CLI.hpp>

#include <asio.hpp>
#include <utility>
#include <wirecall.hpp>

asio::awaitable<void> listener(std::uint16_t port) {
    auto executor = co_await asio::this_coro::executor;

    try {

        asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 5678);
        asio::ip::tcp::acceptor acceptor(executor, ep);
        for (;;) {
            asio::ip::tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
            co_spawn(executor, libusb::proxy::serve(std::move(socket)), asio::detached);
        }

    } catch(std::exception & ex) {
        std::cout << "Uncaught exception: " << ex.what() << "\n";
    }
}

int main(int argc, char **argv) {
    CLI::App app{"libusb proxy server"};

    std::uint16_t port = 5678;
    app.add_option("-p,--port", port, "Port to listen");

    CLI11_PARSE(app, argc, argv);

    libusb_init(NULL);

    std::thread{[]{
        int completed;
        while(true) {
            libusb_handle_events_completed(NULL, &completed);
        }
    }}.detach();

    try {
        asio::thread_pool io_context(10);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto){ io_context.stop(); });

        co_spawn(io_context, listener(port), asio::detached);

        io_context.join();
    } catch (std::exception& e) {
        std::printf("Exception: %s\n", e.what());
    }
}