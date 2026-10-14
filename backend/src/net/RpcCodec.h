/**
 * @file            RpcCodec.h
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
#include <RaftCore/Interfaces/Transport.h> // RequestVoteArgs/Reply, AppendEntriesArgs/Reply

#include <optional>    // std::optional - decode failure
#include <string>      // std::string - encoded message bytes
#include <string_view> // std::string_view - zero-copy decode input
// clang-format on

namespace RaftKV::Net {

/// HTTP paths of the two peer RPCs (POST, binary body in both directions).
inline constexpr const char* kRequestVotePath = "/raft/request-vote";
inline constexpr const char* kAppendEntriesPath = "/raft/append-entries";

// Binary wire format for the four RPC messages, carried as HTTP bodies
// (Content-Type: application/octet-stream). All integers are little-endian;
// strings and payloads are length-prefixed (u32). Every message starts with a
// one-byte format version (currently 1) so a future change can be told apart
// from corruption.
//
//   RequestVoteArgs:    ver, term u64, candidateId, lastLogIndex u64, lastLogTerm u64
//   RequestVoteReply:   ver, term u64, voteGranted u8
//   AppendEntriesArgs:  ver, term u64, leaderId, prevLogIndex u64, prevLogTerm u64,
//                       leaderCommit u64, entryCount u32,
//                       entryCount x { term u64, payload }
//   AppendEntriesReply: ver, term u64, success u8, matchIndex u64
//
// A decoder returns `std::nullopt` for anything that isn't exactly one
// well-formed message: wrong version, truncation, trailing bytes, or an entry
// count larger than the remaining bytes could possibly hold (so a hostile or
// corrupt count can't trigger a huge allocation).

[[nodiscard]] std::string encode(const RaftCore::RequestVoteArgs& message);
[[nodiscard]] std::string encode(const RaftCore::RequestVoteReply& message);
[[nodiscard]] std::string encode(const RaftCore::AppendEntriesArgs& message);
[[nodiscard]] std::string encode(const RaftCore::AppendEntriesReply& message);

[[nodiscard]] std::optional<RaftCore::RequestVoteArgs>
decodeRequestVoteArgs(std::string_view bytes);
[[nodiscard]] std::optional<RaftCore::RequestVoteReply>
decodeRequestVoteReply(std::string_view bytes);
[[nodiscard]] std::optional<RaftCore::AppendEntriesArgs>
decodeAppendEntriesArgs(std::string_view bytes);
[[nodiscard]] std::optional<RaftCore::AppendEntriesReply>
decodeAppendEntriesReply(std::string_view bytes);

} // namespace RaftKV::Net
