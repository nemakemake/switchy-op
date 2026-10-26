// Thin socket layer: Winsock on Windows, BSD sockets elsewhere (the Switch included).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace sw::net {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kBad = INVALID_SOCKET;
#else
using Socket = int;
inline constexpr Socket kBad = -1;
#endif

// Winsock needs starting on Windows; elsewhere this does nothing (the Switch app calls
// socketInitializeDefault itself before using the network).
bool startup();

void close_socket(Socket socket);
bool set_nonblocking(Socket socket);

// True when the last send or receive failed only because it would block.
bool would_block();

// Blocking connect to an IPv4 address or name. Returns kBad on failure.
Socket connect_tcp(const std::string& host, std::uint16_t port);

// A UDP socket "connected" to the peer, so plain send works.
Socket connect_udp(const std::string& host, std::uint16_t port);

// Return the byte count, 0 on orderly close (recv), or -1 on error / would block.
long send_some(Socket socket, const void* data, std::size_t length);
long recv_some(Socket socket, void* data, std::size_t length);

}  // namespace sw::net
