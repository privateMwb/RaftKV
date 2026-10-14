/**
 * @file            PeerAddress.h
 *
 * @date            2026-10-7
 *
 * @version         1.0.0
 *
 * @copyright       Copyright (c) 2026 privateMwb
 *                  All rights reserved.
 *                  https://github.com/privateMwb/RaftKV
 *
 * @attention       This source is released under the MIT license
 *                  SPDX-License-Identifier: MIT
 *                  <http://opensource.org/licenses/MIT>
 */

#pragma once

// clang-format off
#include <cstdint> // uint16_t - TCP port
#include <string>  // std::string - node id and host
// clang-format on

namespace RaftKV::Net {

/// @brief Where to reach one other node in the (static) cluster.
struct PeerAddress {
    /// The peer's RaftCore NodeId.
    std::string id;
    /// IPv4 address in dotted-decimal notation (FalconHTTP's HttpClient does
    /// not resolve hostnames).
    std::string host;
    uint16_t port = 0;
    /// Base URL a *client* should use to reach this node, e.g. `http://localhost:8102`
    /// (a trailing slash is ignored). Empty means "derive it from host and port".
    /// It matters whenever the address peers use is not one a client can reach: a
    /// network-emulating proxy, or a container-internal IP. A follower puts this in
    /// the `Location` of its 307 redirect to the leader.
    std::string publicUrl;
};

} // namespace RaftKV::Net
