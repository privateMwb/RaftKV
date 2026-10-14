import type { ClusterView, Member, NodeState, Role } from "./types.ts";

const ROLES: readonly Role[] = ["leader", "candidate", "follower"];

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function count(value: unknown): number | null {
  return typeof value === "number" && Number.isInteger(value) && value >= 0 ? value : null;
}

export function parseNodeState(value: unknown): NodeState | null {
  if (!isRecord(value)) return null;

  const { id, role, leader } = value;
  const term = count(value.term);
  const commitIndex = count(value.commitIndex);
  const lastApplied = count(value.lastApplied);
  const lastLogIndex = count(value.lastLogIndex);
  const keys = count(value.keys);

  if (typeof id !== "string" || !ROLES.includes(role as Role)) return null;
  if (leader !== null && typeof leader !== "string") return null;
  if (
    term === null ||
    commitIndex === null ||
    lastApplied === null ||
    lastLogIndex === null ||
    keys === null
  )
    return null;

  return {
    id,
    role: role as Role,
    term,
    leader,
    commitIndex,
    lastApplied,
    lastLogIndex,
    keys,
  };
}

/** Validates one `cluster` event payload; null if it isn't a well-formed view. */
export function parseClusterView(json: string): ClusterView | null {
  let value: unknown;
  try {
    value = JSON.parse(json);
  } catch {
    return null;
  }

  if (!isRecord(value) || !Array.isArray(value.members)) return null;

  const term = count(value.term);
  const { observer, leader, quorum } = value;
  if (term === null || typeof observer !== "string" || typeof quorum !== "boolean") return null;
  if (leader !== null && typeof leader !== "string") return null;

  const members: Member[] = [];
  for (const raw of value.members) {
    if (!isRecord(raw) || typeof raw.id !== "string" || typeof raw.up !== "boolean") return null;

    const state = raw.state === null ? null : parseNodeState(raw.state);
    if (raw.up && state === null) return null; // "up" without a report is malformed.

    members.push({ id: raw.id, up: raw.up, state: raw.up ? state : null });
  }

  return { observer, term, leader, quorum, members };
}
