import { parseClusterView } from "./parse.ts";
import { SseParser } from "./sse.ts";
import type { ClusterView, NodeRef, StreamStatus } from "./types.ts";

export interface ClusterStreamOptions {
  /** Every node, in preference order. The stream sticks to one and fails over to the next. */
  nodes: NodeRef[];
  onView: (view: ClusterView, via: NodeRef) => void;
  onStatus: (status: StreamStatus) => void;
  fetchFn?: typeof fetch;
  /** No bytes for this long (the server pings every 2 s) means the node is gone. */
  staleMs?: number;
  /** Pause after every node in a row failed, growing up to the max. */
  retryMinMs?: number;
  retryMaxMs?: number;
}

/**
 * Follows `/cluster/events` on whichever node answers, and moves to another node
 * when the current one stops (closed, errored, or silent for too long). The cluster
 * view is pushed to `onView` the moment the server sends it: no polling.
 */
export class ClusterStream {
  private readonly options: Required<Omit<ClusterStreamOptions, "nodes" | "onView" | "onStatus">> &
    Pick<ClusterStreamOptions, "nodes" | "onView" | "onStatus">;
  private running = false;
  private abort: AbortController | null = null;
  private wake: (() => void) | null = null;
  private runId = 0;

  constructor(options: ClusterStreamOptions) {
    // Defaults are applied per key with `??`, not by spreading `options` over them:
    // a caller that passes `fetchFn: undefined` must get the default, not undefined.
    this.options = {
      nodes: options.nodes,
      onView: options.onView,
      onStatus: options.onStatus,
      fetchFn: options.fetchFn ?? ((...args) => fetch(...args)),
      staleMs: options.staleMs ?? 6000,
      retryMinMs: options.retryMinMs ?? 300,
      retryMaxMs: options.retryMaxMs ?? 3000,
    };
  }

  start(): void {
    if (this.running) return;
    this.running = true;
    const id = ++this.runId;
    void this.loop(id);
  }

  stop(): void {
    this.running = false;
    this.abort?.abort();
    this.wake?.();
  }

  private alive(id: number): boolean {
    return this.running && this.runId === id;
  }

  private async loop(id: number): Promise<void> {
    const { nodes, onStatus } = this.options;
    if (nodes.length === 0) {
      onStatus({ state: "offline", via: null });
      return;
    }

    let cursor = 0;
    let failedInARow = 0;
    let delay = this.options.retryMinMs;

    onStatus({ state: "connecting", via: nodes[0] });

    while (this.alive(id)) {
      const node = nodes[cursor % nodes.length];
      const gotData = await this.follow(node, id);
      if (!this.alive(id)) return;

      if (gotData) {
        // It worked for a while: retry the same node first, with a fresh backoff.
        failedInARow = 0;
        delay = this.options.retryMinMs;
      } else {
        cursor += 1;
        failedInARow += 1;
      }

      if (failedInARow >= nodes.length) {
        onStatus({ state: "offline", via: node });
        await this.sleep(delay);
        delay = Math.min(delay * 2, this.options.retryMaxMs);
        failedInARow = 0;
      } else {
        onStatus({ state: "connecting", via: nodes[cursor % nodes.length] });
        await this.sleep(failedInARow === 0 ? 150 : 50);
      }
    }
  }

  private sleep(ms: number): Promise<void> {
    return new Promise((resolve) => {
      const timer = setTimeout(resolve, ms);
      this.wake = () => {
        clearTimeout(timer);
        resolve();
      };
    });
  }

  /** Streams from one node until it ends. Returns whether any view arrived. */
  private async follow(node: NodeRef, id: number): Promise<boolean> {
    const controller = new AbortController();
    this.abort = controller;

    let watchdog: ReturnType<typeof setTimeout> | undefined;
    const arm = () => {
      clearTimeout(watchdog);
      watchdog = setTimeout(() => controller.abort(), this.options.staleMs);
    };

    let gotData = false;

    try {
      arm();
      const response = await this.options.fetchFn(`${node.url}/cluster/events`, {
        headers: { Accept: "text/event-stream" },
        cache: "no-store",
        signal: controller.signal,
      });
      if (!response.ok || !response.body) return false;

      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      const parser = new SseParser();

      while (this.alive(id)) {
        const { done, value } = await reader.read();
        if (done) break;

        arm(); // Any bytes, pings included, prove the node is alive.

        for (const message of parser.feed(decoder.decode(value, { stream: true }))) {
          if (message.event !== "cluster") continue;

          const view = parseClusterView(message.data);
          if (!view) continue;

          if (!gotData) this.options.onStatus({ state: "live", via: node });
          gotData = true;
          this.options.onView(view, node);
        }
      }
    } catch {
      // Network error, abort, or a body that died mid-read: all mean "try another node".
    } finally {
      clearTimeout(watchdog);
      controller.abort(); // Release the connection if we got here by returning early.
    }

    return gotData;
  }
}
