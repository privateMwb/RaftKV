/**
 * @file RpcCodec.cpp
 * @brief Binary wire codec for the four Raft RPC messages.
 */

// ============================================================
// Implementation for the RaftKV::Net RPC codec.
// ============================================================
//
//  Sections:
//   1. Byte Writer & Reader
//   2. Encoding
//   3. Decoding
//
// ============================================================

// clang-format off
#include "RpcCodec.h" // encode()/decode*() declarations this file implements

#include <cstdint>    // std::uint8_t, std::uint32_t, std::uint64_t
// clang-format on

namespace RaftKV::Net {

using namespace RaftCore;

// ============================================================
//  Section 1 — Byte Writer & Reader
// ============================================================

namespace {

constexpr std::uint8_t kFormatVersion = 1;

// Smallest possible encoded log entry: term (8) + payload length (4).
constexpr std::size_t kMinEntryBytes = 12;

class Writer {
  public:
    Writer() {
        u8(kFormatVersion);
    }

    void u8(std::uint8_t value) {
        out_.push_back(static_cast<char>(value));
    }

    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            out_.push_back(static_cast<char>((value >> shift) & 0xFF));
    }

    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            out_.push_back(static_cast<char>((value >> shift) & 0xFF));
    }

    void string(const std::string& text) {
        u32(static_cast<std::uint32_t>(text.size()));
        out_ += text;
    }

    void payload(const Vector<std::uint8_t>& bytes) {
        u32(static_cast<std::uint32_t>(bytes.size()));
        for (std::size_t i = 0; i < bytes.size(); ++i)
            out_.push_back(static_cast<char>(bytes[i]));
    }

    std::string take() {
        return std::move(out_);
    }

  private:
    std::string out_;
};

class Reader {
  public:
    explicit Reader(std::string_view data) : data_(data) {}

    /// Consumes and checks the leading version byte.
    bool begin() {
        std::uint8_t version = 0;
        return u8(version) && version == kFormatVersion;
    }

    bool u8(std::uint8_t& out) {
        if (remaining() < 1)
            return false;
        out = static_cast<std::uint8_t>(data_[pos_++]);
        return true;
    }

    bool u32(std::uint32_t& out) {
        if (remaining() < 4)
            return false;
        out = 0;
        for (int i = 0; i < 4; ++i)
            out |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[pos_ + i])) << (8 * i);
        pos_ += 4;
        return true;
    }

    bool u64(std::uint64_t& out) {
        if (remaining() < 8)
            return false;
        out = 0;
        for (int i = 0; i < 8; ++i)
            out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[pos_ + i])) << (8 * i);
        pos_ += 8;
        return true;
    }

    bool boolean(bool& out) {
        std::uint8_t raw = 0;
        if (!u8(raw) || raw > 1)
            return false;
        out = raw == 1;
        return true;
    }

    bool string(std::string& out) {
        std::uint32_t length = 0;
        if (!u32(length) || remaining() < length)
            return false;
        out.assign(data_.substr(pos_, length));
        pos_ += length;
        return true;
    }

    bool payload(Vector<std::uint8_t>& out) {
        std::uint32_t length = 0;
        if (!u32(length) || remaining() < length)
            return false;

        Vector<std::uint8_t> bytes;
        bytes.reserve(length);
        for (std::uint32_t i = 0; i < length; ++i)
            bytes.push_back(static_cast<std::uint8_t>(data_[pos_ + i]));
        out = std::move(bytes);
        pos_ += length;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const {
        return data_.size() - pos_;
    }

    /// A message must use every byte it was given.
    [[nodiscard]] bool finished() const {
        return pos_ == data_.size();
    }

  private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

} // namespace

// ============================================================
//  Section 2 — Encoding
// ============================================================

std::string encode(const RequestVoteArgs& message) {
    Writer writer;
    writer.u64(message.term);
    writer.string(message.candidateId);
    writer.u64(message.lastLogIndex);
    writer.u64(message.lastLogTerm);
    return writer.take();
}

std::string encode(const RequestVoteReply& message) {
    Writer writer;
    writer.u64(message.term);
    writer.u8(message.voteGranted ? 1 : 0);
    return writer.take();
}

std::string encode(const AppendEntriesArgs& message) {
    Writer writer;
    writer.u64(message.term);
    writer.string(message.leaderId);
    writer.u64(message.prevLogIndex);
    writer.u64(message.prevLogTerm);
    writer.u64(message.leaderCommit);
    writer.u32(static_cast<std::uint32_t>(message.entries.size()));

    for (std::size_t i = 0; i < message.entries.size(); ++i) {
        writer.u64(message.entries[i].term);
        writer.payload(message.entries[i].payload);
    }

    return writer.take();
}

std::string encode(const AppendEntriesReply& message) {
    Writer writer;
    writer.u64(message.term);
    writer.u8(message.success ? 1 : 0);
    writer.u64(message.matchIndex);
    return writer.take();
}

// ============================================================
//  Section 3 — Decoding
// ============================================================

std::optional<RequestVoteArgs> decodeRequestVoteArgs(std::string_view bytes) {
    Reader reader(bytes);
    RequestVoteArgs message;

    if (!reader.begin() || !reader.u64(message.term) || !reader.string(message.candidateId) ||
        !reader.u64(message.lastLogIndex) || !reader.u64(message.lastLogTerm) || !reader.finished())
        return std::nullopt;

    return message;
}

std::optional<RequestVoteReply> decodeRequestVoteReply(std::string_view bytes) {
    Reader reader(bytes);
    RequestVoteReply message;

    if (!reader.begin() || !reader.u64(message.term) || !reader.boolean(message.voteGranted) ||
        !reader.finished())
        return std::nullopt;

    return message;
}

std::optional<AppendEntriesArgs> decodeAppendEntriesArgs(std::string_view bytes) {
    Reader reader(bytes);
    AppendEntriesArgs message;
    std::uint32_t count = 0;

    if (!reader.begin() || !reader.u64(message.term) || !reader.string(message.leaderId) ||
        !reader.u64(message.prevLogIndex) || !reader.u64(message.prevLogTerm) ||
        !reader.u64(message.leaderCommit) || !reader.u32(count))
        return std::nullopt;

    // Reject impossible counts before allocating anything for them.
    if (count > reader.remaining() / kMinEntryBytes)
        return std::nullopt;

    message.entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        LogEntry entry;
        if (!reader.u64(entry.term) || !reader.payload(entry.payload))
            return std::nullopt;
        message.entries.push_back(std::move(entry));
    }

    if (!reader.finished())
        return std::nullopt;

    return message;
}

std::optional<AppendEntriesReply> decodeAppendEntriesReply(std::string_view bytes) {
    Reader reader(bytes);
    AppendEntriesReply message;

    if (!reader.begin() || !reader.u64(message.term) || !reader.boolean(message.success) ||
        !reader.u64(message.matchIndex) || !reader.finished())
        return std::nullopt;

    return message;
}

} // namespace RaftKV::Net
