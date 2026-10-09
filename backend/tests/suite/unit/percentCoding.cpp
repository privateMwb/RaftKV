// Unit PercentCoding Test Suite
// Verifies key path-segment encoding: any byte string survives a trip
// through percentEncode then percentDecode, and malformed escapes are rejected.
//
// Covers:
// - Unreserved characters pass through unchanged
// - '/', spaces, '%', '+', NUL, and high bytes are escaped
// - Every byte value round-trips
// - Lowercase and uppercase hex both decode
// - '+' is literal in a path segment (not a space)
// - Truncated and non-hex escapes are rejected

#include <support/framework.h>

#include "net/PercentCoding.h"

using namespace RaftKV::Net;

// Verifies plain text is left alone.
static void unreserved_pass_through() {
    CHK(percentEncode("Abc-123_.~") == "Abc-123_.~");
    CHK(percentDecode("Abc-123_.~") == std::optional<std::string>("Abc-123_.~"));
}

// Verifies reserved and special bytes are escaped.
static void special_bytes_escaped() {
    CHK(percentEncode("a/b c") == "a%2Fb%20c");
    CHK(percentEncode("100%+") == "100%25%2B");
    CHK(percentEncode(std::string("\0\xff", 2)) == "%00%FF");
}

// Verifies all 256 byte values round-trip, alone and together.
static void every_byte_round_trips() {
    std::string all;
    for (int value = 0; value < 256; ++value) {
        const std::string single(1, static_cast<char>(value));
        CHK(percentDecode(percentEncode(single)) == std::optional<std::string>(single));
        all.push_back(static_cast<char>(value));
    }

    CHK(percentDecode(percentEncode(all)) == std::optional<std::string>(all));
}

// Verifies hex case is accepted either way and '+' stays a plus.
static void decode_details() {
    CHK(percentDecode("%2f%2F") == std::optional<std::string>("//"));
    CHK(percentDecode("a+b") == std::optional<std::string>("a+b"));
    CHK(percentDecode("%41%42") == std::optional<std::string>("AB"));
    CHK(percentDecode("") == std::optional<std::string>(""));
}

// Verifies broken escapes are refused.
static void malformed_escapes_rejected() {
    CHK(!percentDecode("%").has_value());
    CHK(!percentDecode("%4").has_value());
    CHK(!percentDecode("abc%").has_value());
    CHK(!percentDecode("%zz").has_value());
    CHK(!percentDecode("%4g").has_value());
    CHK(!percentDecode("ok%2").has_value());
}

static void run_tests() {
    RUN(unreserved_pass_through);
    RUN(special_bytes_escaped);
    RUN(every_byte_round_trips);
    RUN(decode_details);
    RUN(malformed_escapes_rejected);
}

REGISTER_TEST_SUITE();
