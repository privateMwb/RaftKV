/**
 * @file NodeSnapshot.cpp
 * @brief NodeSnapshot JSON encoding and parsing.
 */

// clang-format off
#include "NodeSnapshot.h" // NodeSnapshot, toJson, parseSnapshot — what this file implements

#include <JsonPro/Json.h> // JsonPro::Json - parsing peers' /status replies

#include <cmath>          // std::floor - integral-number check
#include <cstdint>        // std::uint64_t
// clang-format on

namespace RaftKV::Node {

const char* roleName(RaftCore::Role role) noexcept {
    switch (role) {
    case RaftCore::Role::Leader:
        return "leader";
    case RaftCore::Role::Candidate:
        return "candidate";
    case RaftCore::Role::Follower:
        return "follower";
    }
    return "unknown";
}

std::optional<RaftCore::Role> parseRole(std::string_view name) noexcept {
    if (name == "leader")
        return RaftCore::Role::Leader;
    if (name == "candidate")
        return RaftCore::Role::Candidate;
    if (name == "follower")
        return RaftCore::Role::Follower;
    return std::nullopt;
}

std::string jsonQuote(const std::string& text) {
    static constexpr char kHex[] = "0123456789abcdef";

    std::string out = "\"";
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (byte < 0x20) {
            out += "\\u00";
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string toJson(const NodeSnapshot& snapshot) {
    std::string json = "{";
    json += "\"id\":" + jsonQuote(snapshot.id);
    json += ",\"role\":" + jsonQuote(roleName(snapshot.role));
    json += ",\"term\":" + std::to_string(snapshot.term);
    json += ",\"leader\":" + (snapshot.leader ? jsonQuote(*snapshot.leader) : std::string("null"));
    json += ",\"commitIndex\":" + std::to_string(snapshot.commitIndex);
    json += ",\"lastApplied\":" + std::to_string(snapshot.lastApplied);
    json += ",\"lastLogIndex\":" + std::to_string(snapshot.lastLogIndex);
    json += ",\"keys\":" + std::to_string(snapshot.keys);
    json += "}";
    return json;
}

namespace {

// A JSON number that is a non-negative integer small enough to be exact in a double.
bool readCount(const JsonPro::Json& object, const char* key, std::uint64_t& out) {
    if (!object.contains(key) || !object.at(key).isNumber())
        return false;

    const double value = object.at(key).asNumber();
    if (value < 0 || value > 9007199254740992.0 || std::floor(value) != value)
        return false;

    out = static_cast<std::uint64_t>(value);
    return true;
}

} // namespace

std::optional<NodeSnapshot> parseSnapshot(std::string_view json) {
    try {
        const JsonPro::Json root = JsonPro::Json::parse(json);
        if (!root.isObject() || !root.contains("id") || !root.at("id").isString() ||
            !root.contains("role") || !root.at("role").isString() || !root.contains("leader"))
            return std::nullopt;

        NodeSnapshot snapshot;
        snapshot.id = root.at("id").asString();

        const auto role = parseRole(root.at("role").asString());
        if (!role)
            return std::nullopt;
        snapshot.role = *role;

        const JsonPro::Json& leader = root.at("leader");
        if (leader.isString())
            snapshot.leader = leader.asString();
        else if (!leader.isNull())
            return std::nullopt;

        std::uint64_t keys = 0;
        if (!readCount(root, "term", snapshot.term) ||
            !readCount(root, "commitIndex", snapshot.commitIndex) ||
            !readCount(root, "lastApplied", snapshot.lastApplied) ||
            !readCount(root, "lastLogIndex", snapshot.lastLogIndex) || !readCount(root, "keys", keys))
            return std::nullopt;
        snapshot.keys = static_cast<std::size_t>(keys);

        return snapshot;
    } catch (const JsonPro::JsonException&) {
        return std::nullopt; // Malformed JSON or a type mismatch.
    }
}

} // namespace RaftKV::Node
