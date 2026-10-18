// Keeps a public demo cluster from staying broken. Visitors can crash nodes and cut links
// on purpose, and may walk away mid-experiment; if the cluster has been unhealthy for
// `resetAfterMs` straight, this restarts any stopped node and removes every network fault,
// so the next visitor finds a working cluster.

/**
 * Healthy means every node is running and the network has no delay, isolation, or
 * partition. `nodes` and `network` are the launcher's GET /nodes and GET /network replies.
 */
export function isHealthy(nodes, network) {
  const allRunning = nodes.nodes.every((node) => node.running);
  const networkClean =
    !network.enabled ||
    (network.delayMs === 0 && network.isolated.length === 0 && network.blocked.length === 0);
  return allRunning && networkClean;
}

export class Watchdog {
  #launcherUrl;
  #resetAfterMs;
  #fetch;
  #log;
  #now;
  #unhealthySince = null;
  #timer = null;

  constructor({ launcherUrl, resetAfterMs, fetchFn = fetch, log = () => {}, now = Date.now }) {
    this.#launcherUrl = launcherUrl.replace(/\/+$/, "");
    this.#resetAfterMs = resetAfterMs;
    this.#fetch = fetchFn;
    this.#log = log;
    this.#now = now;
  }

  start(intervalMs) {
    this.#timer = setInterval(() => void this.tick(), intervalMs);
    this.#timer.unref();
  }

  stop() {
    clearInterval(this.#timer);
  }

  async #get(path) {
    const response = await this.#fetch(`${this.#launcherUrl}${path}`, {
      signal: AbortSignal.timeout(3000),
    });
    if (!response.ok) throw new Error(`${path} answered ${response.status}`);
    return response.json();
  }

  async #post(path) {
    const response = await this.#fetch(`${this.#launcherUrl}${path}`, {
      method: "POST",
      signal: AbortSignal.timeout(5000),
    });
    if (!response.ok) throw new Error(`${path} answered ${response.status}`);
  }

  /** One check. Returns "healthy", "degraded", "repaired", or "unreachable". */
  async tick() {
    let nodes;
    let network;
    try {
      [nodes, network] = await Promise.all([this.#get("/nodes"), this.#get("/network")]);
    } catch {
      return "unreachable"; // the launcher itself is down; its supervisor deals with that
    }

    if (isHealthy(nodes, network)) {
      this.#unhealthySince = null;
      return "healthy";
    }

    const now = this.#now();
    this.#unhealthySince ??= now;
    if (now - this.#unhealthySince < this.#resetAfterMs) return "degraded";

    this.#log("cluster unhealthy for too long: removing faults and restarting stopped nodes");
    try {
      if (network.enabled) await this.#post("/network/reset");
      for (const node of nodes.nodes)
        if (!node.running) await this.#post(`/nodes/${encodeURIComponent(node.id)}/start`);
    } catch (error) {
      this.#log(`repair failed: ${error.message}`);
      return "degraded";
    }
    this.#unhealthySince = null;
    return "repaired";
  }
}
