// Unit RpcCodec Test Suite
// Verifies the binary wire format for the four Raft RPC messages.
//
// Covers:
// - Every message round-trips field for field
// - AppendEntries with many entries, empty payloads, and binary payloads
// - A heartbeat (zero entries) round-trips
// - Wrong version, truncation at every byte, and trailing bytes are rejected
// - An absurd entry count is rejected without allocating for it
// - A boolean byte other than 0/1 is rejected

#include <support/framework.h>

#include "net/RpcCodec.h"

using namespace RaftKV::Net;
using namespace RaftCore;

static Vector<std::uint8_t> bytes(const std::string& text) {
    Vector<std::uint8_t> out;
    for (char c : text)
        out.push_back(static_cast<std::uint8_t>(c));
    return out;
}

static std::string text(const Vector<std::uint8_t>& payload) {
    std::string out;
    for (std::size_t i = 0; i < payload.size(); ++i)
        out.push_back(static_cast<char>(payload[i]));
    return out;
}

// Verifies RequestVote args and reply survive a round trip.
static void request_vote_round_trips() {
    RequestVoteArgs args;
    args.term = 9;
    args.candidateId = "node-3";
    args.lastLogIndex = 41;
    args.lastLogTerm = 8;

    const auto decodedArgs = decodeRequestVoteArgs(encode(args));
    CHK(decodedArgs.has_value());
    CHK(decodedArgs->term == 9);
    CHK(decodedArgs->candidateId == "node-3");
    CHK(decodedArgs->lastLogIndex == 41);
    CHK(decodedArgs->lastLogTerm == 8);

    RequestVoteReply reply;
    reply.term = 9;
    reply.voteGranted = true;

    const auto decodedReply = decodeRequestVoteReply(encode(reply));
    CHK(decodedReply.has_value());
    CHK(decodedReply->term == 9);
    CHK(decodedReply->voteGranted);
}

// Verifies AppendEntries args with mixed entries round-trip exactly.
static void append_entries_round_trips() {
    AppendEntriesArgs args;
    args.term = 4;
    args.leaderId = "node-1";
    args.prevLogIndex = 10;
    args.prevLogTerm = 3;
    args.leaderCommit = 9;

    LogEntry noop;
    noop.term = 4; // empty payload: the election no-op.
    args.entries.push_back(noop);

    LogEntry command;
    command.term = 4;
    command.payload = bytes(std::string("a\0b\xff", 4));
    args.entries.push_back(command);

    const auto decoded = decodeAppendEntriesArgs(encode(args));
    CHK(decoded.has_value());
    CHK(decoded->term == 4);
    CHK(decoded->leaderId == "node-1");
    CHK(decoded->prevLogIndex == 10);
    CHK(decoded->prevLogTerm == 3);
    CHK(decoded->leaderCommit == 9);
    CHK(decoded->entries.size() == 2);
    if (decoded->entries.size() == 2) {
        CHK(decoded->entries[0].payload.size() == 0);
        CHK(text(decoded->entries[1].payload) == std::string("a\0b\xff", 4));
    }

    AppendEntriesReply reply;
    reply.term = 4;
    reply.success = true;
    reply.matchIndex = 12;

    const auto decodedReply = decodeAppendEntriesReply(encode(reply));
    CHK(decodedReply.has_value());
    CHK(decodedReply->success);
    CHK(decodedReply->matchIndex == 12);
}

// Verifies a heartbeat (no entries) and a large batch both round-trip.
static void heartbeat_and_large_batch() {
    AppendEntriesArgs heartbeat;
    heartbeat.term = 1;
    heartbeat.leaderId = "n";
    const auto decodedHeartbeat = decodeAppendEntriesArgs(encode(heartbeat));
    CHK(decodedHeartbeat.has_value());
    CHK(decodedHeartbeat->entries.size() == 0);

    AppendEntriesArgs batch;
    batch.term = 2;
    batch.leaderId = "n";
    for (int i = 0; i < 500; ++i) {
        LogEntry entry;
        entry.term = 2;
        entry.payload = bytes("entry-" + std::to_string(i));
        batch.entries.push_back(entry);
    }

    const auto decodedBatch = decodeAppendEntriesArgs(encode(batch));
    CHK(decodedBatch.has_value());
    CHK(decodedBatch->entries.size() == 500);
    if (decodedBatch->entries.size() == 500)
        CHK(text(decodedBatch->entries[499].payload) == "entry-499");
}

// Verifies a wrong version byte is rejected.
static void wrong_version_rejected() {
    RequestVoteReply reply;
    reply.term = 1;
    std::string wire = encode(reply);
    wire[0] = 2;

    CHK(!decodeRequestVoteReply(wire).has_value());
}

// Verifies every proper prefix of a valid message is rejected.
static void truncation_rejected() {
    AppendEntriesArgs args;
    args.term = 5;
    args.leaderId = "node-2";
    LogEntry entry;
    entry.term = 5;
    entry.payload = bytes("payload");
    args.entries.push_back(entry);

    const std::string wire = encode(args);
    CHK(decodeAppendEntriesArgs(wire).has_value());

    bool anyAccepted = false;
    for (std::size_t length = 0; length < wire.size(); ++length)
        if (decodeAppendEntriesArgs(std::string_view(wire).substr(0, length)).has_value())
            anyAccepted = true;

    CHK(!anyAccepted);
}

// Verifies extra bytes after a complete message are rejected.
static void trailing_bytes_rejected() {
    AppendEntriesReply reply;
    reply.term = 1;
    CHK(!decodeAppendEntriesReply(encode(reply) + "x").has_value());

    RequestVoteArgs args;
    args.candidateId = "a";
    CHK(!decodeRequestVoteArgs(encode(args) + std::string(1, '\0')).has_value());
}

// Verifies a huge declared entry count is rejected, not allocated.
static void absurd_entry_count_rejected() {
    AppendEntriesArgs args;
    args.term = 1;
    args.leaderId = "n";
    std::string wire = encode(args);

    // The u32 entry count is the last 4 bytes of a zero-entry message.
    wire[wire.size() - 4] = '\xff';
    wire[wire.size() - 3] = '\xff';
    wire[wire.size() - 2] = '\xff';
    wire[wire.size() - 1] = '\x7f';

    CHK(!decodeAppendEntriesArgs(wire).has_value());
}

// Verifies a boolean byte other than 0 or 1 is rejected.
static void bad_boolean_rejected() {
    RequestVoteReply reply;
    std::string wire = encode(reply);
    wire[wire.size() - 1] = 2;

    CHK(!decodeRequestVoteReply(wire).has_value());
}

static void run_tests() {
    RUN(request_vote_round_trips);
    RUN(append_entries_round_trips);
    RUN(heartbeat_and_large_batch);
    RUN(wrong_version_rejected);
    RUN(truncation_rejected);
    RUN(trailing_bytes_rejected);
    RUN(absurd_entry_count_rejected);
    RUN(bad_boolean_rejected);
}

REGISTER_TEST_SUITE();
