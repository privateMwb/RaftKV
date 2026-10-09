# backend

C++ backend for RaftKV. Built with CMake; the four dependencies live as git
submodules under `external/` and are pulled in with `add_subdirectory`.

`src/main.cpp` runs one node: a CLI (`--id --port --peer --data --no-repl`) over a `KvNode`, plus a stdin REPL. App code is
built as the `raftkv_core` static library, shared by `raftkv_backend` and
`raftkv_tests` (option `RAFTKV_BUILD_TESTS`, default ON).

## Design notes

- **One mutex around RaftNode.** RaftCore is not thread-safe; `KvNode`
  serializes the tick loop and client writes with a single mutex instead of an
  inbox queue. Reads of the map use the state machine's own reader lock. Phase
  2's RPC handlers take the same mutex (network sends must not block while
  holding it).
- **Writes block until applied**, then verify the entry at that index still
  carries the proposed term, so a write overwritten by a new leader reports
  `LostLeadership` instead of a false `Ok`.
- **Peer RPC is HTTP + a binary body.** `RequestVote`/`AppendEntries` are
  `POST /raft/request-vote` and `/raft/append-entries` (`net/RpcCodec.h` documents
  the wire format, versioned and length-checked). `HttpTransport` never blocks
  RaftNode: a send enqueues onto a per-peer sender thread (ordered, one call in
  flight per peer, 100 ms connect / 300 ms I/O timeouts, bounded queue that drops
  the oldest). Any failure just drops the RPC; Raft's heartbeat and election
  retries are the recovery path. Replies re-enter RaftNode under the same mutex.
- **Static cluster.** Peers come from `--peer`; there is no membership change.
- **Linearizable reads (Raft ReadIndex), with a stale opt-out.** A plain `GET`
  is served by the leader only after (1) it has committed an entry of its own
  term, (2) a majority has acknowledged an AppendEntries *sent after the read
  began*, proving no newer leader exists, and (3) its map has applied up to the
  commit index captured at the start. It costs about one heartbeat plus a round
  trip and adds nothing to the log. Without a quorum the read is refused (`503`)
  rather than answered from possibly stale data; `?stale=1` is the explicit way
  to read a node's local map. Leases would be faster but are unsafe here:
  RaftCore grants votes without leader-stickiness, so a new leader can appear
  before an old one's lease expires.
- **Client API on the peer port.** `net/ClientApi` registers `/kv/:key`;
  followers answer `307` (RFC 9110: same method and body) toward the leader's
  configured address, or `503` when none is known. The key is one
  percent-decoded path segment; the 1 KiB key and 64 KiB value limits are
  set by durability, not by FalconHTTP's 10 MiB body limit (next point).
- **Rate limiting.** ThrottlePro on `/kv/*` only (never peer RPC or `/status`).
  FalconHTTP's `HttpRequest` exposes no client address, so the key is the first
  `X-Forwarded-For` entry and, without a proxy setting it, every client shares
  one `unknown` bucket (the same degradation DnsLint accepts). Exposing the peer
  address in FalconHTTP would fix it properly.
- **fsync stalls must stay under the election timeout.** RaftNode is
  single-threaded behind one mutex, and a write fsyncs its WAL entry (leader and
  followers) while holding it, which also pauses ticking and heartbeats. If the
  leader's fsync outlasts the election timeout (150-300 ms by default),
  followers elect a new leader mid-write and the write fails with
  `LostLeadership`. This was reproduced on a phone: a 1 MiB value never
  committed cleanly, and emulating 3 MB/s flash (an `LD_PRELOAD` fsync shim)
  reproduces it, even with the mitigations below. **The 64 KiB value cap is
  what prevents it**; `--max-value-bytes` raises it, and then the election
  timeout must be raised with it. Two smaller mitigations are on: after a stall
  the tick loop skips the missed ticks instead of replaying them in a burst
  (which would fire the node's own election timer spuriously), and peer RPC
  timeouts grow with the body size so a follower's slow fsync doesn't drop the
  reply. They help follower-side stalls; they do not rescue a leader that stalls
  its own heartbeats. The real fix would be doing WAL appends outside the node
  lock, which RaftCore's single-threaded design does not allow.
- **Live state is pushed, not polled.** `KvNode` recomputes a small
  `NodeSnapshot` at the end of every region that can change state (tick, RPC,
  reply, write) and bumps a version only when it really differs; SSE handlers
  sleep on a condition variable (`waitForChange`) and wake exactly then, so an
  idle cluster costs nothing but a `ping` every 2 s. `/events` is that stream.
- **The cluster view is assembled per node.** `ClusterMonitor` holds one watcher
  for the node itself (instant, via the same condition variable) and one thread
  per peer that polls the peer's `GET /status` every 150 ms (down after 2
  consecutive failures). The node's own changes therefore appear immediately and
  a peer's within about one poll interval. This is the one place peers are
  polled; browsers are never polled. The leader shown is the up member that
  leads in the *highest term*, so a deposed leader that hasn't noticed yet does
  not win. A peer-to-peer SSE subscription would make peer changes instant too,
  at the cost of a streaming client and two pinned server threads per node.
- **Streams share the server pool.** Each open SSE stream occupies a worker
  thread until the client leaves (noticed on the next send, at most one
  keep-alive away) or the node stops (`NodeServer::stop()` wakes them all).
  `--threads` sizes the pool; at most half may be streams.
- **Restart replays from index 1.** RaftCore's `lastApplied` is volatile; the
  map is rebuilt from the WAL once the new term's no-op commits.

## Building

Out-of-source build, from `backend/`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is `build/raftkv_backend`. If you rename that target, update
`BINARY_NAME` in `../Dockerfile` too.

## Known build notes

- FalconHTTP and MiniDatabase emit `-Winterference-size` warnings
  (`std::hardware_destructive_interference_size`) from their vendored
  ThreadPool libraries. Upstream issue, harmless here.
- Each submodule vendors its own copies of shared libraries (VectorPro, ...).
  The current set links without duplicate-symbol errors; if a future
  submodule update introduces one, fix it in the submodule, not here.

## Known limits (Phase 2)

- **Read latency is about one heartbeat** (the leader waits for a post-read
  AppendEntries round). A RaftCore `heartbeatNow()` would cut it to a round trip.
- **No request deduplication.** A write that returns `503` after a timeout may
  still commit, so a blind retry can apply it twice (harmless for `PUT` of the
  same value, not for anything non-idempotent you build on top).
- **No snapshots or log compaction.** A follower that was down for a long time is
  caught up by one AppendEntries carrying its whole backlog; that body must fit
  FalconHTTP's 10 MiB limit.
- **One TCP connection per RPC** (FalconHTTP's server is `Connection: close`).
  Fine for a handful of nodes; it is the first thing to change for throughput.
