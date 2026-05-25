// SPDX-License-Identifier: GPL-3.0
//
// Wire protocol between the Zygisk module (running inside the isolated
// network namespace as the target app process) and the pinner's transparent
// TCP proxy listener (also living in the isolated netns, but with a thread
// holding the host netns fd so it can dial out on behalf of the app).
//
// The app side connects via plain TCP to 127.0.0.1:<proxy_port>, then sends a
// fixed-format handshake describing the original destination.  The pinner
// reads the handshake, switches to host netns, dials the destination, and
// starts splicing bytes between the two sides.
//
// All multi-byte integers are network byte order.  The handshake is exactly
// 28 bytes regardless of address family so the pinner can always read it
// with a single recv loop without first inspecting the family byte.
//
//     offset   size   field
//     0        1      version (currently 1)
//     1        1      family  (4 = AF_INET, 6 = AF_INET6)
//     2        2      port    (network byte order, copied from sockaddr)
//     4        24     address bytes (sockaddr_in.sin_addr lives in [4..8],
//                     sockaddr_in6.sin6_addr lives in [4..20], remainder
//                     is unused/zero)
//
// After the handshake the proxy responds with a single status byte:
//   'N' = success, splicing has begun
//   'E' = failure, errno follows as 4-byte network-order integer, then the
//         proxy closes the connection.
//
// On failure the app's connect() emulation must propagate that errno back to
// the caller.  On success the app's connect() returns 0 and the app then
// reads/writes its TLS records on the same fd as if it were the real peer.

#pragma once

#include <cstdint>

namespace scene_netns {

constexpr uint8_t kProxyHandshakeVersion = 1;
constexpr uint8_t kProxyFamilyV4 = 4;
constexpr uint8_t kProxyFamilyV6 = 6;
constexpr std::size_t kProxyHandshakeSize = 28;
constexpr uint8_t kProxyStatusOk = 'N';
constexpr uint8_t kProxyStatusErr = 'E';

}  // namespace scene_netns
