/**
 * @file PercentCoding.cpp
 * @brief Percent-encoding for key path segments.
 */

// clang-format off
#include "PercentCoding.h" // percentDecode, percentEncode — what this file implements
// clang-format on

namespace RaftKV::Net {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

} // namespace

std::optional<std::string> percentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }

        if (i + 2 >= text.size())
            return std::nullopt; // fewer than two characters follow the '%'.

        const int high = hexValue(text[i + 1]);
        const int low = hexValue(text[i + 2]);
        if (high < 0 || low < 0)
            return std::nullopt;

        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }

    return out;
}

std::string percentEncode(const std::string& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(bytes.size());

    for (char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (isUnreserved(byte)) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }

    return out;
}

} // namespace RaftKV::Net
