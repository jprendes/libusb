#pragma once

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <concepts>
#include <exception>
#include <filesystem>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace libusb::proxy {

struct endpoint {
  private:
    std::variant<
        asio::local::stream_protocol::endpoint,
        asio::ip::tcp::endpoint
    > m_endpoint;

  public:
    template <typename... Args>
    endpoint(Args&&... args) : m_endpoint{std::forward<Args>(args)...} {}

    endpoint() = default;
    endpoint(endpoint &) = default;
    endpoint(endpoint const &) = default;
    endpoint(endpoint &&) = default;

    auto & as_variant() {
        return m_endpoint;
    }

    std::string to_string() {
        std::stringstream ss;
        std::visit([&ss](auto & ep) {
            ss << ep;
        }, m_endpoint);
        return ss.str();
    }
};

inline asio::awaitable<std::vector<endpoint>> parse_uri(std::string address) {
    using namespace std::literals;

    auto executor = co_await asio::this_coro::executor;

    std::regex regex(R"xx((?:tcp://)?(\[([^\]]+?)\]|[^:]+)(?::([0-9]+))?/?|(?:local://)(.+))xx");

    std::smatch match;
    std::regex_match(address, match, regex);

    if (match.empty()) {
        throw std::runtime_error("invalid address " + address);
    } else if (match[4].length() > 0) {
        std::string path = match[4];
        co_return std::vector<endpoint>{ asio::local::stream_protocol::endpoint(path) };
    } else {
        std::string host = match[2].length() > 0 ? match[2] : match[1];
        std::string port = match[3];
        if (port.empty()) port = "0";

        if (host.starts_with("[") && host.ends_with("]")) host = host.substr(1, host.length() - 2);

        asio::ip::tcp::resolver resolver{executor};
        auto resolution = co_await resolver.async_resolve(host, port, asio::ip::tcp::resolver::numeric_service, asio::use_awaitable);
        if (resolution.empty()) {
            throw std::runtime_error("can't resolve " + address);
        }
        co_return std::vector<endpoint>(resolution.begin(), resolution.end());
    }
}

inline asio::awaitable<void> wait_all(std::span<asio::awaitable<void>> awaitables) {
    using namespace asio::experimental::awaitable_operators;
    if (awaitables.empty()) {
        co_return;
    } else if (awaitables.size() == 1) {
        co_await std::move(awaitables[0]);
    } else {
        co_await (std::move(awaitables[0]) && wait_all(awaitables.subspan(1)));
    }
}

inline asio::awaitable<void> wait_one(std::span<asio::awaitable<void>> awaitables) {
    using namespace asio::experimental::awaitable_operators;
    if (awaitables.empty()) {
        co_return;
    } else if (awaitables.size() == 1) {
        co_await std::move(awaitables[0]);
    } else {
        co_await (std::move(awaitables[0]) || wait_all(awaitables.subspan(1)));
    }
}

struct acceptor {
  private:
    using any_acceptor = std::variant<
        asio::local::stream_protocol::acceptor,
        asio::ip::tcp::acceptor
    >;
    any_acceptor m_acceptor;

  public:
    template <typename executor_type>
    acceptor(executor_type const & executor, endpoint ep)
      : m_acceptor(std::visit([&executor]<typename endpoint_type>(endpoint_type & ep) -> any_acceptor {
            using acceptor_type = typename endpoint_type::protocol_type::acceptor;
            auto acceptor = acceptor_type{executor};
            acceptor.open(ep.protocol());
            acceptor.set_option(typename acceptor_type::reuse_address{true});
            acceptor.bind(ep);
            acceptor.listen();
            return acceptor;
        }, ep.as_variant()))
    {}

    ~acceptor() {
        std::visit([]<typename acceptor_type>(acceptor_type & acceptor) {
            if constexpr (std::same_as<acceptor_type, asio::local::stream_protocol::acceptor>) {
                if (acceptor.is_open()) {
                    std::filesystem::remove(acceptor.local_endpoint().path());
                }
            }
        }, m_acceptor);
    }

    asio::awaitable<asio::generic::stream_protocol::socket> accept() {
        co_return co_await std::visit([](auto & acceptor) -> asio::awaitable<asio::generic::stream_protocol::socket> {
            co_return co_await acceptor.async_accept(asio::use_awaitable);
        }, m_acceptor);
    }
};

inline asio::awaitable<asio::generic::stream_protocol::socket> connect(endpoint ep) {
    co_return co_await std::visit([]<typename endpoint_type>(endpoint_type ep) -> asio::awaitable<asio::generic::stream_protocol::socket> {
        using socket_type = typename endpoint_type::protocol_type::socket;
        auto executor = co_await asio::this_coro::executor;
        socket_type socket{executor, ep.protocol()};
        co_await socket.async_connect(ep, asio::use_awaitable);
        co_return socket;
    }, std::move(ep.as_variant()));
}

}