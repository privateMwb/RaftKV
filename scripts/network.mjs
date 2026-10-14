// Network fault injection for the RaftKV demo launcher.
//
// Every node-to-node link goes through a small TCP proxy owned by the launcher, so
// delay and partitions are applied to the *real* traffic between real processes
// (RequestVote, AppendEntries, the peers' /status polls), without touching the
// backend. The dashboard's own connections to the nodes do not go through here,
// so it always sees the truth, even for a node that is cut off from its peers.
//
//   delay      every chunk on every link waits `delayMs` before being forwarded
//              (one way: a round trip costs twice that).
//   partition  a blocked link BLACK-HOLES: the connection is accepted and then
//              nothing ever comes back, like dropped packets. The caller sees
//              timeouts, not instant refusals, which is what a real partition does.

import net from "node:net";

const BLACKHOLE_IDLE_MS = 30_000;
const MAX_DELAY_MS = 2000;

export class NetworkEmulator {
  #host;
  #proxyBase;
  #nodes;
  #links = new Map(); // "a>b" -> { from, to, port, server, connections:Set }
  #blocked = new Set(); // "a>b"
  #delayMs = 0;

  /**
   * @param {{ nodes: { id: string, port: number }[], proxyBase: number, host?: string }} options
   *   `nodes` are the real node ports; the proxy for the link a -> b listens on
   *   `proxyBase + 10 * index(a) + index(b)`.
   */
  constructor({ nodes, proxyBase, host = "127.0.0.1" }) {
    if (nodes.length > 10) throw new Error("at most 10 nodes");
    this.#nodes = nodes;
    this.#proxyBase = proxyBase;
    this.#host = host;
  }

  /** The port node `from` must use to reach node `to`. */
  proxyPort(from, to) {
    const a = this.#nodes.findIndex((node) => node.id === from);
    const b = this.#nodes.findIndex((node) => node.id === to);
    if (a < 0 || b < 0 || a === b) throw new Error(`no link ${from} -> ${to}`);
    return this.#proxyBase + 10 * a + b;
  }

  async start() {
    const listening = [];

    for (const from of this.#nodes) {
      for (const to of this.#nodes) {
        if (from.id === to.id) continue;

        const link = {
          from: from.id,
          to: to.id,
          targetPort: to.port,
          port: this.proxyPort(from.id, to.id),
          connections: new Set(),
          server: null,
        };
        link.server = net.createServer((client) => this.#accept(link, client));
        this.#links.set(`${from.id}>${to.id}`, link);

        listening.push(
          new Promise((resolve, reject) => {
            link.server.once("error", reject);
            link.server.listen(link.port, this.#host, resolve);
          }),
        );
      }
    }

    await Promise.all(listening);
  }

  async stop() {
    const closing = [];

    for (const link of this.#links.values()) {
      this.#sever(link);
      closing.push(
        new Promise((resolve) => link.server.close(() => resolve())),
      );
    }

    await Promise.all(closing);
    this.#links.clear();
  }

  // ------------------------------------------------------------ controls

  get delayMs() {
    return this.#delayMs;
  }

  /** One-way delay added to every chunk on every link, clamped to 0..2000 ms. */
  setDelay(ms) {
    if (!Number.isFinite(ms)) throw new Error("delay must be a number");
    this.#delayMs = Math.min(Math.max(Math.round(ms), 0), MAX_DELAY_MS);
  }

  isBlocked(from, to) {
    return this.#blocked.has(`${from}>${to}`);
  }

  /** Cuts every link to and from `id`. */
  isolate(id) {
    this.#requireNode(id);
    for (const other of this.#nodes) {
      if (other.id === id) continue;
      this.#block(id, other.id);
      this.#block(other.id, id);
    }
  }

  /** Restores every link to and from `id`. */
  reconnect(id) {
    this.#requireNode(id);
    for (const other of this.#nodes) {
      if (other.id === id) continue;
      this.#blocked.delete(`${id}>${other.id}`);
      this.#blocked.delete(`${other.id}>${id}`);
    }
  }

  /**
   * Splits the cluster into groups that can only talk within themselves, e.g.
   * `[["node-1", "node-2"], ["node-3"]]`. Nodes not listed form one more group.
   */
  partition(groups) {
    const groupOf = new Map();
    groups.forEach((group, index) =>
      group.forEach((id) => {
        this.#requireNode(id);
        groupOf.set(id, index);
      }),
    );

    this.#blocked.clear();
    for (const a of this.#nodes) {
      for (const b of this.#nodes) {
        if (a.id === b.id) continue;
        if ((groupOf.get(a.id) ?? -1) !== (groupOf.get(b.id) ?? -1))
          this.#block(a.id, b.id);
      }
    }
  }

  /** Removes every partition (delay is left alone). */
  heal() {
    this.#blocked.clear();
  }

  /** Removes partitions and delay. */
  reset() {
    this.heal();
    this.#delayMs = 0;
  }

  state() {
    const isolated = this.#nodes
      .map((node) => node.id)
      .filter((id) =>
        this.#nodes.every(
          (other) =>
            other.id === id ||
            (this.isBlocked(id, other.id) && this.isBlocked(other.id, id)),
        ),
      );

    return {
      enabled: true,
      delayMs: this.#delayMs,
      // With a single node there is nothing to be cut off from.
      isolated: this.#nodes.length > 1 ? isolated : [],
      blocked: [...this.#blocked].sort().map((key) => key.split(">")),
    };
  }

  // ------------------------------------------------------------ internals

  #requireNode(id) {
    if (!this.#nodes.some((node) => node.id === id))
      throw new Error(`unknown node ${id}`);
  }

  #block(from, to) {
    const key = `${from}>${to}`;
    if (this.#blocked.has(key)) return;

    this.#blocked.add(key);
    const link = this.#links.get(key);
    if (link) this.#sever(link); // Existing conversations on a cut link end now.
  }

  #sever(link) {
    for (const connection of link.connections) {
      connection.client.destroy();
      connection.upstream?.destroy();
    }
    link.connections.clear();
  }

  #accept(link, client) {
    const connection = { client, upstream: null };
    link.connections.add(connection);

    client.on("error", () => {});
    client.on("close", () => {
      connection.upstream?.destroy(); // Nobody left to deliver to.
      link.connections.delete(connection);
    });

    if (this.isBlocked(link.from, link.to)) {
      // Black hole: swallow whatever arrives, answer nothing, and let the
      // caller's own timeout fire. Idle ones are reaped so they can't pile up.
      client.on("data", () => {});
      client.setTimeout(BLACKHOLE_IDLE_MS, () => client.destroy());
      return;
    }

    const upstream = net.connect(link.targetPort, this.#host);
    connection.upstream = upstream;
    // A hard failure (refused, reset) drops the client at once. A normal close must
    // NOT: the node answers and closes ("Connection: close") while its reply is
    // still waiting out the delay, so the pipe ends the client only after flushing.
    upstream.on("error", () => client.destroy());

    this.#pipe(client, upstream);
    this.#pipe(upstream, client);
  }

  /** Copies `from` to `to`, holding each chunk for the current delay, in order. */
  #pipe(from, to) {
    const queue = [];
    let timer = null;
    let ending = false;

    const drain = () => {
      timer = null;

      while (queue.length > 0 && queue[0].at <= Date.now()) {
        const { chunk } = queue.shift();
        if (!to.destroyed) to.write(chunk);
      }

      if (queue.length > 0) timer = setTimeout(drain, queue[0].at - Date.now());
      else if (ending && !to.destroyed && !to.writableEnded) to.end();
    };

    from.on("data", (chunk) => {
      // Never earlier than the previous chunk, so a delay change can't reorder bytes.
      const at = Math.max(Date.now() + this.#delayMs, queue.at(-1)?.at ?? 0);
      queue.push({ at, chunk });
      if (timer === null) drain();
    });

    // `end` (clean FIN) and `close` (anything else) both mean no more data is
    // coming; either way, what is already queued is delivered before we end `to`.
    const finish = () => {
      ending = true;
      if (timer === null) drain();
    };
    from.on("end", finish);
    from.on("close", finish);
  }
}
