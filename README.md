# RaftKV

A small distributed key-value store built on [RaftCore](https://github.com/privateMwb/RaftCore),
with a live web dashboard that shows leader election and log replication as
they happen. The centerpiece is a "kill this node" button: stop the leader and
watch the cluster elect a new one in about half a second.

> **Status: Phase 4 — live cluster state.** Three processes elect a leader over
> HTTP, replicate through a durable WAL, survive `kill -9` of the leader, serve a
> `curl`-able key-value API with leader redirects, linearizable reads, and rate
> limiting, and push their state (leader, term, per-node role, log indexes) as
> Server-Sent Events. The dashboard that draws it is Phase 5. Nothing is deployed.

## Stack

| Submodule (`backend/external/`) | Namespace | Role |
|---|---|---|
| `raftcore` | `RaftCore` | Raft consensus: elections, log replication, commit/apply |
| `falconhttp` | `FalconHTTP` | HTTP server for client API and peer RPC, outbound `HttpClient`, SSE |
| `ratethrottle` | `ThrottlePro` | Rate limiting on the client API |
| `minidatabase` | `MiniDB` | `WriteAheadLog` backing each node's Raft log |

## Roadmap

| Phase | Goal |
|---|---|
| 0 | Repo scaffold; one build linking all four libraries (done) |
| 1 | Single node, no networking: RaftCore + WAL + in-memory map state machine (done) |
| 2 | Multi-node: peer RPC over FalconHTTP, static peer list, 3-node cluster (done) |
| 3 | Client API: `GET`/`PUT`/`DELETE` on the leader, follower redirects, rate limiting (done) |
| 4 | Live cluster-state endpoint over SSE (done) |
| 5 | Frontend dashboard with the kill-node button |
| 6 | Stretch: network delay/partition controls, per-node Docker |

## Layout

```
RaftKV/
├── backend/            C++ backend (CMake)
│   ├── external/       The four submodules above
│   ├── src/
│   │   ├── raft/       FilePersistentState, WalStorage (RaftCore adapters)
│   │   ├── kv/         KvCommand codec, KvStateMachine
│   │   ├── net/        HttpTransport, NodeServer, ClientApi, ClusterMonitor, RpcCodec, PercentCoding, PeerAddress
│   │   ├── node/       KvNode (RaftNode + tick loop + writes + peer RPCs), NodeSnapshot
│   │   └── main.cpp    CLI + stdin REPL
│   ├── tests/          Custom-framework suites (unit/, integration/)
│   ├── include/        App headers
│   └── data/           Runtime data (WAL files); mount as a volume in Docker
├── frontend/           Dashboard (Phase 5)
├── docs/
├── Dockerfile
└── init-nested-submodules.sh
```

## Building

Clone with submodules (each library vendors its own nested submodules too):

```bash
git clone --recurse-submodules https://github.com/privateMwb/RaftKV
cd RaftKV/backend
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/raftkv_backend      # stdin REPL: put <k> <v> | get <k> | del <k> | status | quit
./build/raftkv_tests        # test suites (also: `list`, or a suite/category name)
```

State lives under `data/` (`node-1.wal`, `node-1.state`); delete it for a fresh node.

### Running a 3-node cluster

One terminal per node (from `backend/`):

```bash
./build/raftkv_backend --id node-1 --port 8101 --data data/node-1 --peer node-2=127.0.0.1:8102 --peer node-3=127.0.0.1:8103
./build/raftkv_backend --id node-2 --port 8102 --data data/node-2 --peer node-1=127.0.0.1:8101 --peer node-3=127.0.0.1:8103
./build/raftkv_backend --id node-3 --port 8103 --data data/node-3 --peer node-1=127.0.0.1:8101 --peer node-2=127.0.0.1:8102
```

Type `put k v` on whichever node is the leader (a follower answers
`not leader (leader: node-N)`), then `get k` on the others. `status` shows role,
term, and indices; `curl 127.0.0.1:8101/status` returns the same as JSON. Kill
the leader (`Ctrl-C` or `kill -9`), watch another node take over within a few
hundred ms, restart it, and it catches up. Add `--no-repl` to run headless until
SIGINT/SIGTERM. Peer hosts must be IPv4 addresses.

### Client API

The same port serves clients. Keys are one percent-encoded path segment, values
are the raw body (binary-safe):

```bash
curl -L -X PUT --data-binary 'hello' localhost:8102/kv/greeting   # -L follows the follower's 307 to the leader
curl -L localhost:8103/kv/greeting                                # linearizable read
curl -L -X DELETE localhost:8101/kv/greeting
curl "localhost:8103/kv/greeting?stale=1"                         # this node's local copy, no redirect
curl localhost:8101/status                                        # node state as JSON
```

| Status | Meaning |
|---|---|
| `204` / `200` / `404` | write or delete done / value / key absent |
| `307` | not the leader; `Location` is the leader (method and body preserved) |
| `503` + `Retry-After` | no leader known, commit timed out, leadership lost, or no quorum for a read |
| `400` / `413` / `429` | bad key / value over 64 KiB / rate limited (default 600 requests per client per 10 s, `--rate-limit N`, 0 = off; value cap `--max-value-bytes N`) |

A `503` after a timeout does not mean the write failed: it may still commit.
Every `/kv` response carries `X-RaftKV-Node`. A follower that hasn't yet heard
from a newly elected leader answers `503` + `Retry-After: 1`; clients should retry.

### Live cluster state

State is pushed over Server-Sent Events; nothing needs polling:

```bash
curl -N localhost:8101/cluster/events   # the whole cluster, one event per change
curl -N localhost:8101/events           # just this node, one event per change
curl localhost:8101/cluster             # one-shot JSON of the cluster view
```

A stream starts with the current state, then sends an event only when something
changes, plus a `ping` every 2 s while idle. `/cluster/events` events are named
`cluster`:

```json
{"observer":"node-1","term":2,"leader":"node-3","quorum":true,"members":[
  {"id":"node-1","up":true,"state":{"id":"node-1","role":"follower","term":2,"leader":"node-3","commitIndex":9,"lastApplied":9,"lastLogIndex":9,"keys":3}},
  {"id":"node-2","up":false,"state":null},
  {"id":"node-3","up":true,"state":{"id":"node-3","role":"leader","term":2,"...":"..."}}]}
```

Open it on any live node; if that node dies, reconnect to another. For a
dashboard on a different origin start nodes with `--cors-origin http://localhost:5173`
(or `*`). Each open stream pins one server thread (`--threads N`, default 8; at
most half may be streams, extras get `503`).

## License

MIT — see [LICENSE](LICENSE).
