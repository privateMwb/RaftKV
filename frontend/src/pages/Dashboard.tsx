import { EventFeed } from "@/components/EventFeed";
import { Header } from "@/components/Header";
import { KvConsole } from "@/components/KvConsole";
import { LogLanes } from "@/components/LogLanes";
import { NetworkPanel } from "@/components/NetworkPanel";
import { StoryPanel } from "@/components/StoryPanel";
import { Topology } from "@/components/Topology";
import { useCluster } from "@/hooks/useCluster";
import { useLauncher } from "@/hooks/useLauncher";
import { appConfig } from "@/lib/appConfig";
import type { ClusterModel } from "@/lib/model";
import { cn } from "@/lib/utils";

function ConnectionNotice({ model }: { model: ClusterModel }) {
  if (model.liveCount > 0) return null;

  const connecting = model.nodes.some((node) => node.stream === "connecting");
  const tried = appConfig.nodes.map((node) => node.url.replace("http://", "")).join(", ");

  const [tone, title, body] = connecting
    ? ["border-line-2 text-dim", "Connecting to the cluster…", `Trying ${tried}`]
    : [
        "border-down/40 text-down",
        "Can't reach any node",
        `Tried ${tried}. Start the cluster with "npm run launcher", then this page reconnects by itself.`,
      ];

  return (
    <div
      role="status"
      className={cn("mx-4 mt-4 rounded-xl border bg-panel px-4 py-3 text-sm md:mx-6", tone)}
    >
      <b>{title}</b>
      <span className="ml-2 text-mute">{body}</span>
    </div>
  );
}

export default function Dashboard() {
  const { nodes, launcherUrl } = appConfig;
  const { model, events } = useCluster(nodes);
  const launcher = useLauncher(launcherUrl);

  return (
    <div className="flex min-h-dvh flex-col bg-ink text-fg">
      <Header model={model} />
      <ConnectionNotice model={model} />

      <main className="grid flex-1 gap-4 p-4 pb-[max(1rem,env(safe-area-inset-bottom))] md:p-6 lg:grid-cols-[minmax(0,1fr)_360px]">
        <div className="flex min-w-0 flex-col gap-4">
          <Topology model={model} launcher={launcher} />
          <StoryPanel model={model} delayMs={launcher.network.delayMs} />
          <LogLanes model={model} />
        </div>

        <div className="grid min-w-0 content-start gap-4 md:grid-cols-2 lg:grid-cols-1">
          <KvConsole model={model} />
          <NetworkPanel launcher={launcher} />
          <EventFeed events={events} />
        </div>
      </main>
    </div>
  );
}
