/**
 * @file            PercentCoding.h
 *
 * @date            2026-10-8
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
#include <optional> // std::optional - decode failure
#include <string>   // std::string
// clang-format on

namespace RaftKV::Net {

/**
 * @brief Percent-decodes a URL path segment (RFC 3986): `%HH` -> byte.
 *
 * @details Used for the `:key` segment of `/kv/:key`, which FalconHTTP hands
 * over undecoded. `+` is *not* a space here (that rule is for query strings
 * only). Any byte, including NUL, can be produced via `%00`-style escapes.
 *
 * @return The decoded bytes, or `std::nullopt` for a truncated or non-hex
 * escape (`%`, `%4`, `%zz`).
 */
[[nodiscard]] std::optional<std::string> percentDecode(const std::string& text);

/**
 * @brief Percent-encodes arbitrary bytes for use as a path segment.
 * @details Leaves unreserved characters (`A-Z a-z 0-9 - . _ ~`) as they are
 * and escapes everything else, including `/`, so the result is always one
 * segment and decodes back to exactly the input.
 */
[[nodiscard]] std::string percentEncode(const std::string& bytes);

} // namespace RaftKV::Net
