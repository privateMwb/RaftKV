/**
 * @file            NodeSnapshot.h
 *
 * @date            2026-10-9
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
#include <RaftCore/Common/Types.h> // RaftCore::Term, LogIndex, Role (via Types)
#include <RaftCore/Core/NodeState.h> // RaftCore::Role

#include <cstddef>      // std::size_t
#include <optional>     // std::optional - leader, parse failure
#include <string>       // std::string
#include <string_view>  // std::string_view - parse input
// clang-format on

namespace RaftKV::Node {

/**
 * @brief A point-in-time description of one node, as shown to humans and to the
 * dashboard: who it is, what role it plays, how far its log has got.
 *
 * @details Value type; equality is field-wise, which is how KvNode and
 * ClusterMonitor decide whether anything changed worth pushing.
 */
struct NodeSnapshot {
    std::string id;
    RaftCore::Role role = RaftCore::Role::Follower;
    RaftCore::Term term = RaftCore::kNoTerm;
    /// Who this node believes leads the current term (itself if Leader).
    std::optional<std::string> leader;
    RaftCore::LogIndex commitIndex = RaftCore::kNoIndex;
    RaftCore::LogIndex lastApplied = RaftCore::kNoIndex;
    RaftCore::LogIndex lastLogIndex = RaftCore::kNoIndex;
    /// Keys currently in this node's map.
    std::size_t keys = 0;

    [[nodiscard]] bool operator==(const NodeSnapshot&) const = default;
};

/// @return "leader", "candidate", or "follower".
[[nodiscard]] const char* roleName(RaftCore::Role role) noexcept;

/// @return The Role named by @p name, or `std::nullopt` if unrecognized.
[[nodiscard]] std::optional<RaftCore::Role> parseRole(std::string_view name) noexcept;

/// @brief Quotes and escapes @p text as a JSON string literal.
[[nodiscard]] std::string jsonQuote(const std::string& text);

/// @brief Serializes to one JSON object:
/// `{"id":..,"role":..,"term":..,"leader":..|null,"commitIndex":..,"lastApplied":..,"lastLogIndex":..,"keys":..}`.
[[nodiscard]] std::string toJson(const NodeSnapshot& snapshot);

/// @brief Parses toJson()'s output (also tolerates extra fields).
/// @return The snapshot, or `std::nullopt` if @p json is malformed or a field
/// is missing or mistyped.
[[nodiscard]] std::optional<NodeSnapshot> parseSnapshot(std::string_view json);

} // namespace RaftKV::Node
