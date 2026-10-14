import type { NodeState } from "./types.ts";

/**
 * What one lane cell (one log entry) looks like on a node:
 *  - committed: the node has it and it is committed
 *  - pending: the node has it but it is not committed yet
 *  - missing: the node doesn't have it yet (it is behind)
 *  - none: before the log starts (index < 1)
 */
export type CellKind = "committed" | "pending" | "missing" | "none";

export interface Lane {
  cells: CellKind[];
}

/** First and last log index shown when the window ends at `maxIndex`. */
export function laneWindow(maxIndex: number, size: number): { first: number; last: number } {
  const last = Math.max(maxIndex, 0);
  return { first: Math.max(last - size + 1, 1), last };
}

/**
 * Builds the cells for one node over the `size` entries ending at `maxIndex`.
 * `state` may be a *last seen* report for a node that is down.
 */
export function buildLane(state: NodeState | null, maxIndex: number, size: number): Lane {
  const cells: CellKind[] = [];
  const last = Math.max(maxIndex, 0);

  for (let i = 0; i < size; i += 1) {
    const index = last - size + 1 + i;

    if (index < 1) cells.push("none");
    else if (!state) cells.push("missing");
    else if (index <= state.commitIndex) cells.push("committed");
    else if (index <= state.lastLogIndex) cells.push("pending");
    else cells.push("missing");
  }

  return { cells };
}

/**
 * How many entries a node is missing compared with `referenceIndex`, normally the
 * leader's last log index. (Not the longest log: a cut-off stale leader may hold
 * extra uncommitted entries that the real cluster will discard.)
 */
export function entriesBehind(state: NodeState | null, referenceIndex: number): number {
  return Math.max(referenceIndex - (state?.lastLogIndex ?? 0), 0);
}
