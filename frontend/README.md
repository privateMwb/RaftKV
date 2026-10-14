# frontend

The RaftKV dashboard: a live view of the cluster (who leads, each node's term and
log, replication progress), a key-value console, and Kill / Revive buttons that
crash and restart real node processes. Built to work on a phone as well as a
desktop.

React 19, Vite, Tailwind CSS v4, Boxicons. No other runtime dependencies.

## Run it

You need the backend built (see the root README) and Node 22+.

```bash
# terminal 1: start a 3-node cluster and the launcher that can kill/revive it
npm run launcher -- --fresh

# terminal 2: the dashboard
npm install
npm run dev            # http://localhost:5173
```

**On a phone.** On the same device (e.g. Termux plus the phone's browser) just open
`http://localhost:5173`. From a different phone or tablet on the same Wi-Fi:

```bash
npm run launcher -- --host 0.0.0.0       # lets the dashboard reach the launcher
npm run dev:host                         # Vite prints the http://<LAN address>:5173 URL
```

Open that URL on the phone. The dashboard looks for the nodes and the launcher on
whatever host served the page, so no configuration is needed. (`--host 0.0.0.0`
means anyone on your network can kill your demo nodes; use it on a network you trust.)

## How it works

- **Live state is pushed, from every node.** `src/lib/watcher.ts` opens one
  Server-Sent Events stream per node (`GET /cluster/events`, read with `fetch` so it
  controls reconnects and a 6 s silence watchdog; nodes ping every 2 s) and
  `src/lib/model.ts` combines what each node says about itself. Asking every node,
  not one "observer", is what lets a partition be shown truthfully: a node that is
  alive but cut off still reports itself (possibly as a stale leader), which one
  observer would mistake for a dead node. Nothing polls the cluster.
- **Events are derived**, not sent: `src/lib/events.ts` diffs consecutive models into
  "node-3 went down", "node-1 lost contact with its peers", "node-2 became leader",
  "quorum lost".
- **"What's happening"** (`src/lib/story.ts`) turns the model into a plain-language
  explanation: election in progress, stale leader, no majority, a node catching up.
- **Network panel and Isolate buttons** drive the launcher's proxies: a delay slider and
  per-node isolation. The slider's warning zones come from measurement (see
  `docs/ARCHITECTURE.md`).
- **Cards keep their place.** Nodes sit on a ring by index, so a leader election
  recolours cards instead of shuffling them. On a phone they stack in one column.
- **Kill, Revive, Isolate and the delay slider** go through `scripts/launcher.mjs`, a
  small local process that owns the node processes and sits on every node-to-node link. Kill is a real `SIGKILL`: a crash, not a tidy shutdown.
  The one place the dashboard checks in on a timer is the launcher (every 3 s).
- **Key-value writes** go straight to the current leader (avoids a cross-origin
  redirect); the value is sent as the raw request body.

## Configuration

All optional, in `.env.local` (see `.env.example`): `VITE_NODE_HOST`,
`VITE_NODE_PORTS` (default `8101,8102,8103`), `VITE_LAUNCHER_URL`. Nodes started by
the launcher allow any origin (`--cors-origin '*'`); if you start nodes yourself,
pass `--cors-origin http://localhost:5173` (or `'*'`).

## Layout

```
src/
├── lib/          Framework-free logic: stream client, SSE parser, cluster model, event
│                 derivation, explanations, lane and ring layout, KV and launcher
│                 clients, config. Unit-tested.
├── hooks/        useCluster, useLauncher
├── components/   Header, Topology, NodeCard, LogLanes, StoryPanel, NetworkPanel,
│                 KvConsole, EventFeed, Panel
└── pages/        Dashboard
tests/            node:test suites (run TypeScript directly) + an opt-in end-to-end test
```

Responsive behaviour: below `md` (768 px) everything is one column and controls are
44 px tall for touch; at `md` the topology becomes a ring and the side panels sit
in two columns; at `lg` (1024 px) the side panels move to a right-hand column.

## Scripts

| Script | What it does |
|---|---|
| `npm run dev` / `dev:host` | Vite dev server (`dev:host` also listens on the LAN) |
| `npm run build` | Type-check and build to `dist/` |
| `npm run lint` | ESLint |
| `npm test` | Unit, stream and network-emulator tests (Node 22+, no extra dependencies) |
| `npm run test:e2e` | Drives a **running** cluster through the launcher; needs `E2E_LAUNCHER` and `E2E_NODES` (see `tests/e2e.test.ts`) |
| `npm run launcher` | Start the cluster and the kill/revive launcher |
