# How RaftKV works

RaftKV is a small replicated key-value store: three (or more) processes that agree on
every write, keep working when one of them dies, and refuse to answer rather than
answer wrongly when they can't reach each other. This page explains how, and where each
idea lives in the code. The figures in "What the measurements show" were measured on real
processes; the rest describes the design and the code.

## The shape of the system

```
 browser (dashboard)                                    launcher (demo only)
   │  one SSE stream PER NODE  ┐                          │ spawns / SIGKILLs nodes
   │  PUT/GET/DELETE /kv/...   │                          │ proxies node↔node traffic
   ▼                           ▼                          ▼
 ┌──────────┐   AppendEntries / RequestVote (HTTP)  ┌──────────┐
 │  node-1  │◄─────────── via launcher proxies ────►│  node-2  │ ... node-3
 │  Raft +  │                                        │          │
 │  WAL +   │   each node: NodeServer (HTTP, 1 port) │          │
 │  KV map  │   KvNode → RaftNode → WalStorage (disk)│          │
 └──────────┘                                        └──────────┘
```

One node is one process. Inside it:

| Piece | Job | Code |
|---|---|---|
| `RaftNode` | The consensus algorithm: elections, replication, commit | `RaftCore` (library) |
| `WalStorage` | The Raft log on disk; every append is fsynced before it is acknowledged | `backend/src/raft/` over MiniDB's `WriteAheadLog` |
| `FilePersistentState` | `currentTerm` and `votedFor`, rewritten atomically (temp file, fsync, rename) | `backend/src/raft/` |
| `KvStateMachine` | The map that committed commands are applied to | `backend/src/kv/` |
| `KvNode` | Ties them together behind **one mutex**: tick loop, client writes, peer RPCs | `backend/src/node/` |
| `HttpTransport` / `NodeServer` | Peer RPC over HTTP, and the one HTTP port that also serves clients and the live streams | `backend/src/net/` |

## Raft in one page

Raft keeps a *log* of commands identical on every node. Three ideas do the work.

**Terms and leaders.** Time is divided into numbered *terms*. In each term at most one
node is the *leader*; it alone accepts writes and sends `AppendEntries` to the others,
which doubles as a heartbeat. A follower that hears nothing for a randomized 150–300 ms
becomes a *candidate*, adds one to the term, and asks for votes. A candidate that gets
votes from a majority becomes leader. The randomization keeps two candidates from
splitting the vote forever. Each node persists its term and who it voted for, so a
restart can never lead it to vote twice in one term.

**Commit by majority.** A write is appended to the leader's log, sent to the followers,
and counts as *committed* once a majority (2 of 3) has it on disk. Only then is it
applied to the map and acknowledged to the client (`204`). Because any two majorities
overlap, a committed write is present on at least one node of every future majority, so
no future leader can lack it. That is the safety guarantee: **an acknowledged write is
never lost**, however nodes crash.

**Terms fix stale leaders.** A leader that is cut off keeps believing it leads. When it
rejoins and sees a higher term, it steps down, and its *uncommitted* entries (the ones
the majority never saw) are overwritten by the new leader's log.

### What a request does

*Write* (`PUT /kv/key`): the leader appends to its WAL (fsync), sends `AppendEntries`
immediately (not on the next heartbeat), each follower fsyncs and replies, the leader
sees a majority, commits, applies, answers `204`. A follower answers `307` pointing at
the leader (using the node's configured **public URL**, which can differ from the
address peers use: a proxy, a container IP); with no leader known it answers `503`
with `Retry-After`. If a write times out the answer is `503` but the write **may still
commit**; retry only idempotent intents.

*Read* (`GET /kv/key`): linearizable by default using Raft's **ReadIndex**. The leader
(1) checks it has committed an entry from its own term, (2) waits for a majority to
acknowledge an `AppendEntries` *sent after the read began*, which proves no newer leader
exists, and (3) waits until it has applied up to the commit index it captured. Without a
quorum it answers `503` instead of possibly stale data. `?stale=1` reads a node's local
map with none of these checks. Time-based leases would be faster but are unsafe here:
RaftCore grants votes without leader stickiness, so a new leader can appear before an
old one's lease expires.

### Crashes and restarts

On restart a node reloads its term and vote, opens the WAL (truncating a torn trailing
record), and comes back as a follower. Raft's `lastApplied` is volatile, so the map is
rebuilt by replaying the log from index 1 once the node learns the commit index. Nothing
acknowledged is lost: it was fsynced on a majority before the client heard `204`.

## Seeing it: observability

`NodeSnapshot` (role, term, who it thinks leads, commit/applied/last-log index, key
count) is recomputed at the end of every region that can change it and a version is
bumped only on a real difference. Server-Sent Events handlers sleep on a condition
variable and wake exactly then, so an idle cluster costs only a `ping` every 2 s.

- `GET /status`: this node's snapshot.
- `GET /events`: this node's snapshot stream.
- `GET /cluster`, `GET /cluster/events`: this node's **own view** of the cluster: itself
  (instantly) and each peer (a poll of the peer's `/status` every 150 ms; a peer is
  "down" after 2 failed polls).

### Why the dashboard listens to *every* node

A view from one node cannot tell "dead" from "unreachable from me". So the dashboard
opens `/cluster/events` on **each node** and builds one model from what every node says
about itself and about the others (`frontend/src/lib/model.ts`):

- **live**: we are receiving that node's own reports.
- **isolated**: live, but *its own view* sees no majority while other nodes are live
  (the links are broken, not the nodes). A lone survivor of two crashes is *not*
  isolated: its peers are dead, not unreachable.
- **stale leader**: believes it is leader but is not the cluster's leader (the leader is
  the live node with a majority behind it, in the highest term).

That is what makes a partition readable: the cut-off old leader is shown alive, still
claiming leadership, and marked stale, while the other two elect a new one.

## Breaking it on purpose

`scripts/launcher.mjs` owns the node processes and sits in the middle of every
node-to-node connection:

- **Kill** is `SIGKILL`: a real crash; nothing is flushed or closed.
- **Delay** holds every chunk on every node-to-node link for N ms, in order.
- **Isolate** *black-holes* a node's links: connections are accepted and nothing comes
  back, like dropped packets, so callers see timeouts, not instant refusals (which is
  what a real partition does).

The dashboard's own connections bypass the proxies, so it always sees the truth.

## What the measurements show

Real processes through the launcher's proxies (one-way delay on every link):

| One-way delay | Writes | Leadership |
|---|---|---|
| 0 ms | ~8 ms | stable |
| 40 ms | 80–160 ms (one to two round trips) | stable |
| 80 ms and up | fail with `503` | elections every few hundred ms |

Why 80 ms breaks it: a heartbeat round trip (≥160 ms) outlasts the shortest election
timeout (150 ms), and the transport keeps one RPC in flight per peer, so heartbeats
cannot arrive faster than one per round trip. Followers time out, elect, and the cluster
flaps. **Safety holds throughout** (nothing acknowledged is lost, nothing stale is
served); only availability suffers. The rule is the standard one: election timeouts must
be several times the network round trip.

Partition of the leader (isolate it): the other two elect a new leader within a
fraction of a second and keep accepting writes; the cut-off leader stays "leader" of the
old term, its writes and linearizable reads return `503` (never silently lost, never
stale), and after healing it steps down, receives the majority's writes, and the write it
refused was never applied anywhere.

## Decisions and trade-offs

- **One mutex around RaftNode.** RaftCore is single-threaded by design; the tick loop,
  client writes, peer RPCs and reply deliveries all take the same lock. Simple and
  correct. The cost: an fsync inside the lock also pauses heartbeats, so a slow disk
  can look like a dead leader. That is why values are capped at 64 KiB by default
  (a 1 MiB write on a phone's flash caused election churn). Raise the cap only together
  with the election timeout.
- **Peer RPC is HTTP with a small versioned binary body**, one TCP connection per RPC,
  one sender thread per peer. A newer `AppendEntries` replaces an older one still
  queued (it carries everything the older one did); votes are never dropped.
- **Static membership.** The cluster is the `--peer` list; there is no joining or
  removing nodes at runtime.
- **The launcher, not an admin endpoint, kills nodes.** A browser cannot kill a process;
  putting a "kill me" endpoint on every node would be an unauthenticated foot-gun.
- **Rate limiting** (ThrottlePro) covers `/kv/*` only. FalconHTTP's request exposes no
  client address, so clients are keyed by `X-Forwarded-For`, else share one bucket.

## Known limits

No snapshots or log compaction (a long-dead node is caught up from the full log in one
request, which must fit FalconHTTP's 10 MiB body limit). No membership changes. Peer
addresses are dotted IPv4 (the HTTP client does not resolve names). One in-flight RPC per
peer caps heartbeat rate at the round-trip time. Writes that time out may still commit
and there is no request deduplication. The Docker setup (`docker-compose.yml`) has not
been run by its author: see the README.
