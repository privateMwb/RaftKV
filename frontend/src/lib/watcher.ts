import { ClusterStream, type ClusterStreamOptions } from "./clusterStream.ts";
import { deriveEvents } from "./events.ts";
import { buildModel, type ClusterModel, type LastSeen, type NodeObservation } from "./model.ts";
import type { FeedEvent, NodeRef } from "./types.ts";

export interface WatcherOptions {
  nodes: NodeRef[];
  /** Called with the new model whenever it changes, plus any events that change implies. */
  onChange: (model: ClusterModel, events: FeedEvent[]) => void;
  fetchFn?: typeof fetch;
  now?: () => number;
  /**
   * How often to re-evaluate the model with no news from the nodes. Some conclusions
   * depend only on time passing (a node streaming for a full second with no majority
   * is isolated), so they need a periodic look. This is local arithmetic on data we
   * already hold; it does not touch the network.
   */
  checkMs?: number;
  retryMinMs?: number;
  retryMaxMs?: number;
  staleMs?: number;
}

/**
 * Follows every node's own `/cluster/events` stream and keeps the combined
 * ClusterModel up to date. One stream per node, each pushed by that node; nothing
 * is polled. The React hook is a thin wrapper around this class so the same logic
 * runs in the browser and in the node-based tests.
 */
export class ClusterWatcher {
  private readonly options: WatcherOptions;
  private readonly now: () => number;
  private streams: ClusterStream[] = [];
  private observations: Record<string, NodeObservation> = {};
  private lastSeen: Record<string, LastSeen> = {};
  private previous: ClusterModel | null = null;
  private previousKey = "";
  private timer: ReturnType<typeof setInterval> | null = null;
  private running = false;

  constructor(options: WatcherOptions) {
    this.options = options;
    this.now = options.now ?? (() => Date.now());
  }

  /** The current model (computed fresh). */
  get model(): ClusterModel {
    return buildModel(this.options.nodes, this.observations, this.lastSeen, this.now());
  }

  start(): void {
    if (this.running) return;
    this.running = true;

    for (const ref of this.options.nodes) {
      this.observations[ref.id] = { stream: "connecting", view: null, liveSince: null };

      const streamOptions: ClusterStreamOptions = {
        nodes: [ref],
        fetchFn: this.options.fetchFn,
        onStatus: (status) => {
          const before = this.observations[ref.id];
          this.observations[ref.id] = {
            stream: status.state,
            view: before?.view ?? null,
            liveSince: status.state === "live" ? (before?.liveSince ?? this.now()) : null,
          };
          this.refresh();
        },
        onView: (view) => {
          const now = this.now();
          const before = this.observations[ref.id];
          this.observations[ref.id] = {
            stream: "live",
            view,
            liveSince: before?.liveSince ?? now,
          };

          const own = view.members.find((member) => member.id === ref.id)?.state;
          if (own) this.lastSeen[ref.id] = { state: own, at: now };
          this.refresh();
        },
        retryMinMs: this.options.retryMinMs ?? 300,
        retryMaxMs: this.options.retryMaxMs ?? 1500,
      };
      if (this.options.staleMs !== undefined) streamOptions.staleMs = this.options.staleMs;

      const stream = new ClusterStream(streamOptions);
      this.streams.push(stream);
      stream.start();
    }

    this.timer = setInterval(() => this.refresh(), this.options.checkMs ?? 500);
    this.refresh();
  }

  stop(): void {
    this.running = false;
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    for (const stream of this.streams) stream.stop();
    this.streams = [];
  }

  private refresh(): void {
    if (!this.running) return;

    const model = this.model;
    const key = JSON.stringify(model);
    if (key === this.previousKey) return;

    const events = deriveEvents(this.previous, model, this.now());
    this.previous = model;
    this.previousKey = key;
    this.options.onChange(model, events);
  }
}
