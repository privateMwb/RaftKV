import type { ClusterView, NodeRef, NodeState, StreamState } from "./types.ts";

/**
 * How long a node must have been streaming before we believe it is cut off from its
 * peers. A node that has only just (re)started briefly sees nobody because its own
 * monitor hasn't polled anyone yet; that is not a partition.
 */
export const SETTLE_MS = 1000;

/** What we have heard from one node's own `/cluster/events` stream. */
export interface NodeObservation {
  stream: StreamState;
  /** That node's latest view of the cluster (its own state is `members[0]`). */
  view: ClusterView | null;
  /** When this stream last became live; null while it isn't. */
  liveSince: number | null;
}

export interface LastSeen {
  state: NodeState;
  at: number;
}

export interface NodeModel {
  id: string;
  ref: NodeRef;
  stream: StreamState;
  /** We are receiving this node's own reports right now. */
  live: boolean;
  /** Its own latest report: live while `live`, otherwise the last one we saw. */
  state: NodeState | null;
  /** When `state` was last live; null if we never saw this node. */
  lastSeenAt: number | null;
  /** Whether *this node* sees a majority of the cluster up (its own view). */
  hasQuorum: boolean | null;
  /** Which peers this node sees up (its own view), by id. */
  sees: Record<string, boolean>;
  /** Running and reachable by us, but cut off from a majority of its peers. */
  isolated: boolean;
  /** Believes it is the leader, but is not the cluster's leader. */
  staleLeader: boolean;
}

export interface ClusterModel {
  /** Every configured node, in configuration order. */
  nodes: NodeModel[];
  /** The leader that has a majority behind it; null during an election or without quorum. */
  leader: string | null;
  /** Highest term reported by any node. */
  term: number;
  /** A majority of nodes are running and connected to each other: the cluster can commit. */
  quorum: boolean;
  liveCount: number;
  /** Live and not isolated. */
  healthyCount: number;
  majority: number;
}

/**
 * Combines what every node says about itself into one picture of the cluster.
 *
 * Each node is asked directly, so the picture stays truthful in a partition: a node
 * that is alive but cut off still reports its own (possibly stale) state, and its own
 * view shows it has lost the majority. Reading from a single "observer" node cannot
 * tell "dead" from "unreachable from me".
 */
export function buildModel(
  refs: NodeRef[],
  observations: Record<string, NodeObservation | undefined>,
  lastSeen: Record<string, LastSeen | undefined>,
  now: number,
): ClusterModel {
  const total = refs.length;
  const majority = Math.floor(total / 2) + 1;

  const base = refs.map((ref) => {
    const obs = observations[ref.id];
    const view = obs?.view ?? null;
    const live = obs?.stream === "live" && view !== null;
    const self = live ? view.members.find((member) => member.id === ref.id) : undefined;
    const remembered = lastSeen[ref.id];

    return {
      ref,
      obs,
      view,
      live,
      state: (live ? self?.state : null) ?? remembered?.state ?? null,
      lastSeenAt: remembered?.at ?? null,
      sees: live
        ? Object.fromEntries(
            (view?.members ?? [])
              .filter((member) => member.id !== ref.id)
              .map((member) => [member.id, member.up]),
          )
        : {},
    };
  });

  const liveCount = base.filter((node) => node.live).length;

  const nodes: NodeModel[] = base.map((node) => {
    const hasQuorum = node.live && node.view ? node.view.quorum : null;
    const settled = node.obs?.liveSince != null && now - node.obs.liveSince >= SETTLE_MS;

    return {
      id: node.ref.id,
      ref: node.ref,
      stream: node.obs?.stream ?? "connecting",
      live: node.live,
      state: node.state,
      lastSeenAt: node.lastSeenAt,
      hasQuorum,
      sees: node.sees,
      // Alive, sees no majority, yet other nodes are alive too: so it is the
      // *links* that are broken, not the other nodes. (If they were all dead it
      // would simply be a lone survivor, which is a different story.)
      isolated: node.live && hasQuorum === false && liveCount > 1 && settled,
      staleLeader: false,
    };
  });

  const leaders = nodes.filter(
    (node) => node.live && node.hasQuorum === true && node.state?.role === "leader",
  );
  leaders.sort((a, b) => (b.state?.term ?? 0) - (a.state?.term ?? 0));
  const leader = leaders[0]?.id ?? null;

  for (const node of nodes)
    node.staleLeader = node.live && node.state?.role === "leader" && node.id !== leader;

  const healthyCount = nodes.filter((node) => node.live && !node.isolated).length;

  return {
    nodes,
    leader,
    term: Math.max(0, ...nodes.map((node) => node.state?.term ?? 0)),
    quorum: healthyCount >= majority,
    liveCount,
    healthyCount,
    majority,
  };
}
