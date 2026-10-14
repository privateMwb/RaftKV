import { Panel } from "@/components/Panel";
import { buildLane, entriesBehind, laneWindow, type CellKind } from "@/lib/lanes";
import type { ClusterModel } from "@/lib/model";
import { cn } from "@/lib/utils";

const WINDOW = 28;

const CELL: Record<CellKind, string> = {
  committed: "bg-leader",
  pending: "bg-leader/35",
  missing: "bg-line",
  none: "bg-transparent",
};

// A down node's entries are shown grey: they exist on its disk but it isn't taking part.
const CELL_DOWN: Record<CellKind, string> = {
  committed: "bg-follower/45",
  pending: "bg-follower/25",
  missing: "bg-line/60",
  none: "bg-transparent",
};

export function LogLanes({ model }: { model: ClusterModel }) {
  const live = model.nodes.filter((node) => node.live);
  const maxIndex = Math.max(0, ...live.map((node) => node.state?.lastLogIndex ?? 0));
  const { first, last } = laneWindow(maxIndex, WINDOW);

  // "Behind" is measured against the real leader, not the longest log: a cut-off
  // stale leader can hold extra uncommitted entries that will be discarded.
  const leader = model.nodes.find((node) => node.id === model.leader);
  const reference = leader?.state?.lastLogIndex ?? maxIndex;

  return (
    <Panel
      title="Log replication"
      aside={
        <span className="font-mono">
          {last > 0 ? `entries ${first}–${last}` : "no entries yet"}
        </span>
      }
      className="min-h-0 flex-1"
    >
      <ul className="flex flex-col gap-2.5">
        {model.nodes.map((node) => {
          const lane = buildLane(node.state, maxIndex, WINDOW);
          const behind = entriesBehind(node.state, reference);

          const [status, tone] = !node.live
            ? ["down", "text-down"]
            : node.isolated
              ? ["cut off", "text-candidate"]
              : behind > 0
                ? [`behind ${behind}`, "text-candidate"]
                : ["in sync", "text-mute"];

          return (
            <li key={node.id} className="flex items-center gap-3">
              <div className="w-[4.5rem] shrink-0 sm:w-24">
                <div className={cn("font-mono text-xs", node.live ? "text-dim" : "text-down")}>
                  {node.id}
                </div>
                <div className={cn("font-mono text-[10px] whitespace-nowrap", tone)}>{status}</div>
              </div>
              <div
                className="flex h-4 flex-1 gap-[2px]"
                role="img"
                aria-label={`${node.id} log: ${status}`}
              >
                {lane.cells.map((kind, index) => (
                  <span
                    key={index}
                    className={cn(
                      "min-w-0 flex-1 rounded-[2px]",
                      (node.live ? CELL : CELL_DOWN)[kind],
                    )}
                  />
                ))}
              </div>
            </li>
          );
        })}
      </ul>

      <p className="mt-4 flex flex-wrap gap-x-4 gap-y-1 font-mono text-[11px] text-mute">
        <span>solid = committed</span>
        <span>faint = not yet committed</span>
        <span>empty = missing on that node</span>
      </p>
    </Panel>
  );
}
