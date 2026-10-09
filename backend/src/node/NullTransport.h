/**
 * @file            NullTransport.h
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
#include <RaftCore/Interfaces/Transport.h> // RaftCore::Transport (the interface implemented here)
// clang-format on

namespace RaftKV::Node {

/**
 * @brief A Transport for a cluster of one: there is nobody to send to.
 *
 * @details Phase 1 only. A node with an empty peer list never calls the
 * transport; if it ever does, that's a bug, so it reports `IO_ERROR` rather
 * than silently swallowing the RPC. Replaced by an HTTP transport in Phase 2.
 */
class NullTransport final : public RaftCore::Transport {
  public:
    [[nodiscard]] RaftCore::Status
    sendRequestVote(const RaftCore::NodeId&, RaftCore::RequestVoteArgs,
                    FunctionPro::MoveOnlyFunction<void(RaftCore::RequestVoteReply)>) override {
        return RaftCore::Status::IO_ERROR;
    }

    [[nodiscard]] RaftCore::Status
    sendAppendEntries(const RaftCore::NodeId&, RaftCore::AppendEntriesArgs,
                      FunctionPro::MoveOnlyFunction<void(RaftCore::AppendEntriesReply)>) override {
        return RaftCore::Status::IO_ERROR;
    }
};

} // namespace RaftKV::Node
