import type { ClusterModel } from "@/lib/model";
import { cn } from "@/lib/utils";

const chip =
  "flex items-center gap-2 rounded-full border border-line px-3.5 py-1.5 font-mono text-xs whitespace-nowrap text-dim";

export function Header({ model }: { model: ClusterModel }) {
  const total = model.nodes.length;
  const heard = model.nodes.some((node) => node.state !== null);

  return (
    <header className="flex flex-wrap items-center gap-x-5 gap-y-3 border-b border-line px-4 py-3 md:px-6">
      <h1 className="text-lg font-bold tracking-tight">
        <span className="text-leader">Raft</span>KV
      </h1>

      {/* Second row on a phone, inline from md up. */}
      <div className="order-last flex w-full gap-2 overflow-x-auto pb-0.5 md:order-none md:w-auto md:overflow-visible">
        {heard ? (
          <>
            <span className={chip}>
              <span className={cn("size-2 rounded-full", model.quorum ? "bg-ok" : "bg-down")} />
              quorum {model.healthyCount} / {total}
            </span>
            <span className={chip}>
              {model.leader ? (
                <>
                  leader <b className="text-leader">{model.leader}</b>
                </>
              ) : model.quorum ? (
                <span className="text-candidate">no leader · election</span>
              ) : (
                <span className="text-down">no leader</span>
              )}
            </span>
            <span className={chip}>term {model.term}</span>
          </>
        ) : (
          <span className={chip}>waiting for the cluster…</span>
        )}
      </div>

      <LiveIndicator model={model} />
    </header>
  );
}

function LiveIndicator({ model }: { model: ClusterModel }) {
  const total = model.nodes.length;
  const connecting = model.nodes.some((node) => node.stream === "connecting");

  const [dot, text] =
    model.liveCount === total
      ? ["bg-leader animate-ping-ring", `live · ${total} of ${total} nodes`]
      : model.liveCount > 0
        ? ["bg-candidate", `live · ${model.liveCount} of ${total} nodes`]
        : connecting
          ? ["bg-candidate", "connecting…"]
          : ["bg-down", "offline"];

  return (
    <span className={cn(chip, "ml-auto")} role="status">
      <span className={cn("size-2 rounded-full", dot)} />
      {text}
    </span>
  );
}
