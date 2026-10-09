/**
 * @file            KvCommand.h
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
#include <VectorPro/Vector.h> // VectorPro::Vector - the opaque payload type RaftCore replicates
#include <cstddef>            // std::size_t
#include <cstdint>            // std::uint8_t, std::uint32_t
#include <optional>           // std::optional - decode() failure
#include <string>             // std::string - key and value
// clang-format on

namespace RaftKV::Kv {

/// @brief What a replicated command does to the map.
enum class KvOp : std::uint8_t {
    Put = 1,
    Delete = 2,
};

/**
 * @brief One client command, as it travels through the Raft log.
 *
 * @details Wire layout (binary-safe: keys and values may contain any byte,
 * including NUL and newlines):
 *
 *     [op: 1 byte][keyLength: 4 bytes, little-endian][key][value: rest]
 *
 * A Delete carries an empty value. The encoded payload is never empty, which
 * RaftCore requires of propose() (an empty payload is the election no-op).
 */
struct KvCommand {
    KvOp op = KvOp::Put;
    std::string key;
    std::string value;
};

/// @brief Serializes a command into a Raft log payload.
[[nodiscard]] inline VectorPro::Vector<std::uint8_t> encodeCommand(const KvCommand& command) {
    VectorPro::Vector<std::uint8_t> out;
    out.reserve(5 + command.key.size() + command.value.size());

    out.push_back(static_cast<std::uint8_t>(command.op));

    const auto keyLength = static_cast<std::uint32_t>(command.key.size());
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>((keyLength >> shift) & 0xFF));

    for (char c : command.key)
        out.push_back(static_cast<std::uint8_t>(c));
    for (char c : command.value)
        out.push_back(static_cast<std::uint8_t>(c));

    return out;
}

/**
 * @brief Parses a Raft log payload back into a command.
 * @return The command, or `std::nullopt` if the payload is truncated, has an
 * unknown op, or declares a key longer than the payload.
 */
[[nodiscard]] inline std::optional<KvCommand>
decodeCommand(const VectorPro::Vector<std::uint8_t>& payload) {
    if (payload.size() < 5)
        return std::nullopt;

    const std::uint8_t rawOp = payload[0];
    if (rawOp != static_cast<std::uint8_t>(KvOp::Put) &&
        rawOp != static_cast<std::uint8_t>(KvOp::Delete))
        return std::nullopt;

    std::uint32_t keyLength = 0;
    for (int i = 0; i < 4; ++i)
        keyLength |= static_cast<std::uint32_t>(payload[1 + i]) << (8 * i);

    if (keyLength > payload.size() - 5)
        return std::nullopt;

    KvCommand command;
    command.op = static_cast<KvOp>(rawOp);
    command.key.reserve(keyLength);
    for (std::size_t i = 0; i < keyLength; ++i)
        command.key.push_back(static_cast<char>(payload[5 + i]));
    for (std::size_t i = 5 + keyLength; i < payload.size(); ++i)
        command.value.push_back(static_cast<char>(payload[i]));

    return command;
}

} // namespace RaftKV::Kv
