// SPDX-License-Identifier: GPL-3.0
//
// Wire protocol between the Zygisk module (in target app process) and the
// root companion process. The companion runs in the host network namespace
// and lends out fully-connected sockets so the isolated app process can talk
// to the outside world without leaving its own netns.
//
// Protocol is request/response over a single long-lived AF_UNIX SOCK_STREAM.
// Multi-threaded callers must serialise access to that socket.

#pragma once

#include <cstdint>

namespace scene_netns {

// Command identifiers. Sent from the app side to the companion as the first
// byte of every request.
enum ProxyCommand : uint8_t {
    // No payload. Companion responds with SCM_RIGHTS carrying the pinned
    // network namespace fd, plus a single status byte (kStatusOk).
    kCmdFetchNetns = 0,

    // Payload after the command byte:
    //   int32_t domain     (network byte order)
    //   int32_t type       (network byte order)
    //   int32_t protocol   (network byte order)
    //   int32_t addr_len   (network byte order)
    //   uint8_t addr[addr_len]   (raw sockaddr bytes from the caller)
    //   int32_t flags      (network byte order, application-defined)
    //
    // flags bit 0: O_NONBLOCK on the original fd (informational; companion
    //              currently always performs a blocking connect and lets
    //              the caller handle non-blocking semantics by setting the
    //              flag back on the returned fd).
    //
    // Response on success:
    //   uint8_t  status    = kStatusOk
    //   SCM_RIGHTS fd      (already-connected socket living in host netns)
    //
    // Response on failure:
    //   uint8_t  status    = kStatusErr
    //   int32_t  errno_val (network byte order)
    kCmdHostConnect = 1,
};

// Status byte that prefixes every response.
enum ProxyStatus : uint8_t {
    kStatusOk  = 'N',
    kStatusErr = 'E',
};

// Bit flags for kCmdHostConnect.flags.
enum ProxyConnectFlag : uint32_t {
    kConnectFlagNonblock = 1u << 0,
};

}  // namespace scene_netns
