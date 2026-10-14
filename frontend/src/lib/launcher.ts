// Client for scripts/launcher.mjs: a tiny local process that starts and kills the
// real backend processes. A browser can't kill a process, so the dashboard asks it.

export interface LauncherNode {
  id: string;
  port: number;
  running: boolean;
  pid: number | null;
}

export type LauncherAction = "start" | "kill" | "stop";

export async function fetchLauncherNodes(
  baseUrl: string,
  fetchFn: typeof fetch = (...args) => fetch(...args),
): Promise<LauncherNode[]> {
  const response = await fetchFn(`${baseUrl}/nodes`, {
    signal: AbortSignal.timeout(2000),
    cache: "no-store",
  });
  if (!response.ok) throw new Error(`launcher answered ${response.status}`);

  const body: unknown = await response.json();
  const list =
    typeof body === "object" && body !== null && "nodes" in body
      ? (body as { nodes: unknown }).nodes
      : null;
  if (!Array.isArray(list)) throw new Error("unexpected launcher reply");

  return list.filter(
    (node): node is LauncherNode =>
      typeof node === "object" &&
      node !== null &&
      typeof node.id === "string" &&
      typeof node.port === "number" &&
      typeof node.running === "boolean",
  );
}

export async function launcherAction(
  baseUrl: string,
  id: string,
  action: LauncherAction,
  fetchFn: typeof fetch = (...args) => fetch(...args),
): Promise<void> {
  const response = await fetchFn(`${baseUrl}/nodes/${encodeURIComponent(id)}/${action}`, {
    method: "POST",
    signal: AbortSignal.timeout(4000),
  });
  if (!response.ok) throw new Error(`${action} ${id} failed (${response.status})`);
}

// ------------------------------------------------------------ network emulation

/** The launcher's current network faults (see scripts/network.mjs). */
export interface NetworkState {
  /** False when the launcher runs with --no-network: delay and partitions are unavailable. */
  enabled: boolean;
  /** One-way delay added to every node-to-node link. */
  delayMs: number;
  /** Nodes cut off from every other node. */
  isolated: string[];
  blocked: [string, string][];
}

export type NetworkAction =
  | { kind: "delay"; ms: number }
  | { kind: "isolate"; id: string }
  | { kind: "reconnect"; id: string }
  | { kind: "heal" }
  | { kind: "reset" };

export const NO_NETWORK: NetworkState = { enabled: false, delayMs: 0, isolated: [], blocked: [] };

function parseNetwork(body: unknown): NetworkState {
  if (typeof body !== "object" || body === null) throw new Error("unexpected network reply");
  const value = body as Record<string, unknown>;
  if (value.enabled !== true) return NO_NETWORK;

  const isolated = Array.isArray(value.isolated)
    ? value.isolated.filter((id): id is string => typeof id === "string")
    : [];
  const blocked = Array.isArray(value.blocked)
    ? value.blocked.filter(
        (pair): pair is [string, string] =>
          Array.isArray(pair) && pair.length === 2 && pair.every((id) => typeof id === "string"),
      )
    : [];

  return {
    enabled: true,
    delayMs: typeof value.delayMs === "number" ? value.delayMs : 0,
    isolated,
    blocked,
  };
}

export async function fetchNetwork(
  baseUrl: string,
  fetchFn: typeof fetch = (...args) => fetch(...args),
): Promise<NetworkState> {
  const response = await fetchFn(`${baseUrl}/network`, {
    signal: AbortSignal.timeout(2000),
    cache: "no-store",
  });
  if (!response.ok) throw new Error(`launcher answered ${response.status}`);
  return parseNetwork(await response.json());
}

export function networkPath(action: NetworkAction): string {
  switch (action.kind) {
    case "delay":
      return `/network/delay?ms=${Math.round(action.ms)}`;
    case "isolate":
    case "reconnect":
      return `/network/${action.kind}/${encodeURIComponent(action.id)}`;
    case "heal":
    case "reset":
      return `/network/${action.kind}`;
  }
}

export async function networkAction(
  baseUrl: string,
  action: NetworkAction,
  fetchFn: typeof fetch = (...args) => fetch(...args),
): Promise<NetworkState> {
  const response = await fetchFn(`${baseUrl}${networkPath(action)}`, {
    method: "POST",
    signal: AbortSignal.timeout(4000),
  });
  if (!response.ok) throw new Error(`network ${action.kind} failed (${response.status})`);
  return parseNetwork(await response.json());
}
