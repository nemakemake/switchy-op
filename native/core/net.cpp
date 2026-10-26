#include "net.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#if __has_include(<netinet/tcp.h>)
#include <netinet/tcp.h>
#endif
#endif

namespace sw::net {

bool startup() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void close_socket(Socket socket) {
    if (socket == kBad) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool set_nonblocking(Socket socket) {
#ifdef _WIN32
    u_long enable = 1;
    return ioctlsocket(socket, FIONBIO, &enable) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

namespace {

Socket open_to(const std::string& host, std::uint16_t port, int type) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found)
        return kBad;
    Socket socket = kBad;
    for (addrinfo* a = found; a; a = a->ai_next) {
        Socket candidate = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (candidate == kBad) continue;
        if (::connect(candidate, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
            socket = candidate;
            break;
        }
        close_socket(candidate);
    }
    freeaddrinfo(found);
    return socket;
}

}  // namespace

Socket connect_tcp(const std::string& host, std::uint16_t port) {
    Socket socket = open_to(host, port, SOCK_STREAM);
#ifdef TCP_NODELAY
    if (socket != kBad) {
        int on = 1;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
    }
#endif
    return socket;
}

Socket connect_udp(const std::string& host, std::uint16_t port) {
    return open_to(host, port, SOCK_DGRAM);
}

long send_some(Socket socket, const void* data, std::size_t length) {
    return static_cast<long>(::send(socket, static_cast<const char*>(data),
                                    static_cast<int>(length), 0));
}

long recv_some(Socket socket, void* data, std::size_t length) {
    return static_cast<long>(::recv(socket, static_cast<char*>(data), static_cast<int>(length), 0));
}

}  // namespace sw::net
