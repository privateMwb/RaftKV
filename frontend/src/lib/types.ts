// Shapes of the data the RaftKV backend streams from `GET /cluster/events`
// (see backend/src/net/ClusterMonitor.h) plus the few client-side types built on them.

export type Role = "leader" | "candidate" | "follower";

/** One node's own report of itself (the backend's NodeSnapshot). */
export interface NodeState {
  id: string;
  role: Role;
  term: number;
  leader: string | null;
  commitIndex: number;
  lastApplied: number;
  lastLogIndex: number;
  keys: number;
}

/** A node as seen by the node we are streaming from. */
export interface Member {
  id: string;
  up: boolean;
  /** The node's last report; present exactly when `up`. */
  state: NodeState | null;
}

/** The whole cluster as one node sees it. `members[0]` is that node. */
export interface ClusterView {
  observer: string;
  term: number;
  /** The up member leading in the highest term, if any. */
  leader: string | null;
  /** Whether a majority of members are up (the cluster can commit writes). */
  quorum: boolean;
  members: Member[];
}

/** Where to reach one node's HTTP API. */
export interface NodeRef {
  id: string;
  /** Base URL without a trailing slash, e.g. `http://localhost:8101`. */
  url: string;
}

export type StreamState = "connecting" | "live" | "offline";

export interface StreamStatus {
  state: StreamState;
  /** The node the live stream comes from (or the last one tried). */
  via: NodeRef | null;
}

export type FeedTone = "info" | "good" | "warn" | "bad" | "leader";

export interface FeedEvent {
  id: string;
  at: number;
  tone: FeedTone;
  text: string;
}
