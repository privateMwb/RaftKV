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
};

} // namespace RaftKV::Net
