import type { CSSProperties } from "react";
import { formatClock } from "@/lib/format";
import type { NodeModel } from "@/lib/model";
import { cn } from "@/lib/utils";
import { ROLE_STYLE, type Display } from "./roleStyle";

interface NodeCardProps {
  node: NodeModel;
  /** ":8102" */
  port: string;
  launcherOnline: boolean;
  /** Whether the launcher says the process is running (undefined if it doesn't know). */
  running: boolean | undefined;
  busy: boolean;
  /** Is network emulation available, and has the launcher cut this node off? */
  networkEnabled: boolean;
  cutOff: boolean;
  networkBusy: boolean;
  onKill: () => void;
  onRevive: () => void;
  onIsolate: () => void;
  onReconnect: () => void;
  className?: string;
  style?: CSSProperties;
}

function displayOf(node: NodeModel): Display {
  if (node.live && node.state) return node.state.role;
  if (node.stream === "connecting" && node.state === null) return "unknown";
  return "down";
}

export function NodeCard({
  node,
  port,
  launcherOnline,
  running,
  busy,
  networkEnabled,
  cutOff,
  networkBusy,
  onKill,
  onRevive,
  onIsolate,
  onReconnect,
  className,
  style,
}: NodeCardProps) {
  const display = displayOf(node);
  const look = ROLE_STYLE[display];
  const isUp = display !== "down" && display !== "unknown";
  const state = node.state;

  const stats: [string, string][] = [
    ["TERM", state ? String(state.term) : "–"],
    ["COMMIT", state ? String(state.commitIndex) : "–"],
    ["LOG", state ? String(state.lastLogIndex) : "–"],
    ["KEYS", isUp && state ? String(state.keys) : "–"],
  ];

  // Cut off / stale are things we know that the role alone doesn't say.
  const notice = node.isolated
    ? node.staleLeader
      ? "Cut off · stale leader"
      : "Cut off from its peers"
    : node.staleLeader
      ? "Stale leader"
      : null;

  const canKill = launcherOnline && !busy;
  const launcherHint = !launcherOnline
    ? "Start the launcher to enable this (npm run launcher)"
    : undefined;
  const networkHint = !launcherOnline
    ? launcherHint
    : !networkEnabled
      ? "The launcher was started with --no-network"
      : undefined;

  return (
    <article
      aria-label={`${node.id}, ${look.label.toLowerCase()}${node.isolated ? ", cut off" : ""}`}
      className={cn(
        "flex flex-col gap-2.5 rounded-xl border bg-card px-3.5 py-3 transition-colors",
        look.border,
        node.isolated && "border-dashed border-candidate/70 shadow-none",
        className,
      )}
      style={style}
    >
      <div className="flex items-center justify-between gap-2">
        <h3 className="text-base font-bold whitespace-nowrap">{node.id}</h3>
        <span
          className={cn(
            "flex items-center gap-1 rounded-md px-2 py-0.5 text-[10px] font-bold tracking-[0.1em] whitespace-nowrap",
            node.staleLeader ? "bg-candidate/15 text-candidate" : look.badge,
          )}
        >
          {display === "leader" && !node.staleLeader ? (
            <i className="bx bx-crown text-xs" aria-hidden="true" />
          ) : null}
          {node.staleLeader ? "STALE LEADER" : look.label}
        </span>
      </div>

      {notice ? (
        <p className="-mt-1 flex items-center gap-1.5 text-[11px] whitespace-nowrap text-candidate">
          <i className="bx bx-unlink text-sm" aria-hidden="true" />
          {notice}
        </p>
      ) : null}

      <dl className="grid grid-cols-4 gap-1.5">
        {stats.map(([label, value]) => (
          <div
            key={label}
            className="rounded-lg border border-line bg-cell px-1 py-1.5 text-center"
          >
            <dt className="font-mono text-[9px] font-semibold tracking-[0.06em] text-mute">
              {label}
            </dt>
            <dd className={cn("font-mono text-[15px] font-semibold", !isUp && "text-mute")}>
              {value}
            </dd>
          </div>
        ))}
      </dl>

      {isUp ? (
        <div className="grid grid-cols-2 gap-2">
          <ActionButton
            icon={cutOff ? "bx-link" : "bx-unlink"}
            label={cutOff ? "Reconnect" : "Isolate"}
            tone="warn"
            busy={networkBusy}
            disabled={!launcherOnline || !networkEnabled || networkBusy}
            title={networkHint}
            onClick={cutOff ? onReconnect : onIsolate}
          />
          <ActionButton
            icon="bx-power-off"
            label="Kill node"
            tone="danger"
            busy={busy}
            disabled={!canKill}
            title={launcherHint}
            onClick={onKill}
          />
        </div>
      ) : (
        <div className="flex items-center justify-between gap-2">
          <span className="font-mono text-[11px] whitespace-nowrap text-mute">
            {node.lastSeenAt
              ? `last seen ${formatClock(node.lastSeenAt)}`
              : display === "unknown"
                ? "waiting…"
                : "never seen"}
          </span>
          <ActionButton
            icon="bx-play"
            label="Revive"
            tone="ok"
            busy={busy}
            disabled={!canKill || running === true || display === "unknown"}
            title={launcherHint}
            onClick={onRevive}
          />
        </div>
      )}
      <span className="sr-only">{port}</span>
    </article>
  );
}

interface ActionButtonProps {
  icon: string;
  label: string;
  tone: "danger" | "ok" | "warn";
  busy: boolean;
  disabled: boolean;
  title?: string;
  onClick: () => void;
}

function ActionButton({ icon, label, tone, busy, disabled, title, onClick }: ActionButtonProps) {
  return (
    <button
      type="button"
      onClick={onClick}
      disabled={disabled}
      title={title}
      className={cn(
        "flex min-h-11 items-center justify-center gap-1.5 rounded-lg border px-2.5 text-xs font-medium whitespace-nowrap transition-colors md:min-h-8",
        "disabled:cursor-not-allowed disabled:opacity-40",
        tone === "danger" && "border-down/40 text-down enabled:hover:bg-down/10",
        tone === "ok" && "border-ok/40 text-ok enabled:hover:bg-ok/10",
        tone === "warn" && "border-candidate/40 text-candidate enabled:hover:bg-candidate/10",
      )}
    >
      <i className={cn("bx text-sm", busy ? "bx-loader-alt bx-spin" : icon)} aria-hidden="true" />
      {label}
    </button>
  );
}
