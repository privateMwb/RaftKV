import { NodeCard } from "@/components/NodeCard";
import { Panel } from "@/components/Panel";
import { ROLE_STYLE, type Display } from "@/components/roleStyle";
import type { Launcher } from "@/hooks/useLauncher";
import { portOf } from "@/lib/format";
import { ringPositions } from "@/lib/layout";
import type { ClusterModel } from "@/lib/model";

interface TopologyProps {
  model: ClusterModel;
  launcher: Launcher;
}

const LEGEND: Display[] = ["leader", "follower", "candidate", "down"];

export function Topology({ model, launcher }: TopologyProps) {
  const positions = ringPositions(model.nodes.length);
  const leader = model.nodes.find((node) => node.id === model.leader);

  return (
    <Panel
      title="Cluster"
      aside={
        <span className="hidden items-center gap-3.5 sm:flex">
          {LEGEND.map((role) => (
            <span key={role} className="flex items-center gap-1.5">
              <i className={`size-2 rounded-full ${ROLE_STYLE[role].dot}`} />
              {role}
            </span>
          ))}
        </span>
      }
    >
      {!launcher.online ? (
        <p className="mb-3 flex items-start gap-2 rounded-lg border border-line bg-cell px-3 py-2 text-xs text-mute">
          <i className="bx bx-info-circle mt-px text-sm" aria-hidden="true" />
          <span>
            Launcher offline, so Kill, Revive and Isolate are disabled. Run{" "}
            <code className="font-mono text-dim">npm run launcher</code>.
          </span>
        </p>
      ) : null}
      {launcher.error ? (
        <p
          role="alert"
          className="mb-3 rounded-lg border border-down/40 px-3 py-2 text-xs text-down"
        >
          {launcher.error}
        </p>
      ) : null}

      {/* A stack on a phone; a ring with links from the leader from md up. */}
      <div className="relative flex flex-col gap-3 md:block md:h-[430px]">
        {leader ? (
          <svg
            className="pointer-events-none absolute inset-0 hidden size-full md:block"
            viewBox="0 0 100 100"
            preserveAspectRatio="none"
            aria-hidden="true"
          >
            {model.nodes.map((node, index) => {
              if (node.id === leader.id) return null;
              const at = positions[model.nodes.indexOf(leader)];
              const to = positions[index];
              // A link is up when the *leader itself* sees that node, and we can too.
              const linked = leader.sees[node.id] === true && node.live && !node.isolated;

              return (
                <line
                  key={node.id}
                  x1={at.x}
                  y1={at.y}
                  x2={to.x}
                  y2={to.y}
                  vectorEffect="non-scaling-stroke"
                  strokeWidth={2}
                  strokeDasharray={linked ? "8 8" : "4 6"}
                  className={linked ? "animate-flow stroke-leader/70" : "stroke-down/50"}
                />
              );
            })}
          </svg>
        ) : null}

        {model.nodes.map((node, index) => (
          <NodeCard
            key={node.id}
            node={node}
            port={portOf(node.ref.url)}
            launcherOnline={launcher.online}
            running={launcher.nodes[node.id]?.running}
            busy={launcher.busy[node.id] === true}
            networkEnabled={launcher.network.enabled}
            cutOff={launcher.network.isolated.includes(node.id)}
            networkBusy={launcher.networkBusy}
            onKill={() => void launcher.act(node.id, "kill")}
            onRevive={() => void launcher.act(node.id, "start")}
            onIsolate={() => void launcher.changeNetwork({ kind: "isolate", id: node.id })}
            onReconnect={() => void launcher.changeNetwork({ kind: "reconnect", id: node.id })}
            className="md:absolute md:w-[252px] md:-translate-x-1/2 md:-translate-y-1/2"
            style={{ left: `${positions[index].x}%`, top: `${positions[index].y}%` }}
          />
        ))}
      </div>
    </Panel>
  );
}
