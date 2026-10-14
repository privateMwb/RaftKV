import http from "node:http";
import type { AddressInfo } from "node:net";
import {
  buildModel,
  type ClusterModel,
  type LastSeen,
  type NodeObservation,
} from "../src/lib/model.ts";
import type { ClusterView, Member, NodeRef, NodeState, Role } from "../src/lib/types.ts";

export function member(id: string, role: "leader" | "follower" | "candidate", extra = {}): Member {
  return {
    id,
    up: true,
    state: {
      id,
      role,
      term: 4,
      leader: "node-2",
      commitIndex: 10,
      lastApplied: 10,
      lastLogIndex: 10,
      keys: 3,
      ...extra,
    },
  };
}

export function down(id: string): Member {
  return { id, up: false, state: null };
}

export function view(members: Member[], overrides: Partial<ClusterView> = {}): ClusterView {
  const up = members.filter((m) => m.up);
  const leader = up.find((m) => m.state?.role === "leader")?.id ?? null;
  return {
    observer: members[0].id,
    term: 4,
    leader,
    quorum: up.length >= Math.floor(members.length / 2) + 1,
    members,
    ...overrides,
  };
}

export interface FakeNode {
  url: string;
  port: number;
  /** Writes one SSE message to every open stream. */
  send: (event: string, data: string) => void;
  /** Ends every open stream and stops accepting new ones. */
  kill: () => Promise<void>;
  requests: () => number;
}

/** A real HTTP server that behaves like a node's `/cluster/events` stream. */
export async function fakeNode(
  onConnect: (send: (event: string, data: string) => void) => void = () => {},
  listenPort = 0,
): Promise<FakeNode> {
  const streams = new Set<http.ServerResponse>();
  const sockets = new Set<import("node:net").Socket>();
  let requests = 0;

  const send = (event: string, data: string) => {
    for (const res of streams) res.write(`event: ${event}\ndata: ${data}\n\n`);
  };

  const server = http.createServer((req, res) => {
    requests += 1;
    if (req.url !== "/cluster/events") {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-cache" });
    streams.add(res);
    res.on("close", () => streams.delete(res));
    onConnect(send);
  });
  server.on("connection", (socket) => {
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
  });

  await new Promise<void>((resolve) => server.listen(listenPort, "127.0.0.1", resolve));
  const port = (server.address() as AddressInfo).port;

  return {
    url: `http://127.0.0.1:${port}`,
    port,
    send,
    requests: () => requests,
    kill: () =>
      new Promise<void>((resolve) => {
        for (const socket of sockets) socket.destroy();
        server.close(() => resolve());
      }),
  };
}

export async function waitFor(check: () => boolean, ms = 3000): Promise<boolean> {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (check()) return true;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  return check();
}

// ---------------------------------------------------------------- model builders

export interface NodeSpec {
  /** Is its stream live? (false = the dashboard cannot reach it: killed.) */
  live?: boolean;
  role?: Role;
  term?: number;
  /** Peers this node itself sees up (its own view). Default: everyone. */
  sees?: string[];
  lastLogIndex?: number;
  commitIndex?: number;
  keys?: number;
  /** Streaming since this long ago, in ms (default long enough to be settled). */
  liveForMs?: number;
}

export const ids = (n: number) => Array.from({ length: n }, (_, i) => `node-${i + 1}`);
export const refs = (n: number): NodeRef[] =>
  ids(n).map((id, i) => ({ id, url: `http://127.0.0.1:${8101 + i}` }));

/** One node's own cluster view, from its own point of view. */
export function viewFrom(id: string, all: string[], spec: NodeSpec): ClusterView {
  const sees = new Set(spec.sees ?? all.filter((other) => other !== id));
  const role = spec.role ?? "follower";
  const term = spec.term ?? 1;

  const self: NodeState = {
    id,
    role,
    term,
    leader: role === "leader" ? id : null,
    commitIndex: spec.commitIndex ?? 10,
    lastApplied: spec.commitIndex ?? 10,
    lastLogIndex: spec.lastLogIndex ?? 10,
    keys: spec.keys ?? 3,
  };

  const members: Member[] = [
    { id, up: true, state: self },
    ...all
      .filter((other) => other !== id)
      .map((other): Member =>
        sees.has(other)
          ? { id: other, up: true, state: { ...self, id: other, role: "follower", leader: null } }
          : { id: other, up: false, state: null },
      ),
  ];

  const up = members.filter((member) => member.up).length;
  const quorum = up >= Math.floor(all.length / 2) + 1;

  return { observer: id, term, leader: role === "leader" && quorum ? id : null, quorum, members };
}

/**
 * Builds a ClusterModel from per-node specs, the same way the dashboard does: from
 * what each live node says about itself. `specs` is keyed by node id; a missing id is
 * a node that is not live.
 */
export function modelOf(
  count: number,
  specs: Record<string, NodeSpec>,
  now = 100_000,
  seen: Record<string, LastSeen> = {},
): ClusterModel {
  const all = ids(count);
  const observations: Record<string, NodeObservation> = {};
  const lastSeen: Record<string, LastSeen> = { ...seen };

  for (const id of all) {
    const spec = specs[id];
    if (!spec || spec.live === false) {
      observations[id] = { stream: "offline", view: null, liveSince: null };
      continue;
    }

    const view = viewFrom(id, all, spec);
    observations[id] = { stream: "live", view, liveSince: now - (spec.liveForMs ?? 60_000) };
    const own = view.members[0].state;
    if (own) lastSeen[id] = { state: own, at: now };
  }

  return buildModel(refs(count), observations, lastSeen, now);
}
