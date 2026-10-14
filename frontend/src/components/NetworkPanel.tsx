import { useEffect, useId, useRef, useState } from "react";
import { Panel } from "@/components/Panel";
import type { Launcher } from "@/hooks/useLauncher";
import { delayZone } from "@/lib/story";
import { cn } from "@/lib/utils";

const MAX_DELAY = 200;

const ZONE_TEXT = {
  ok: "text-ok",
  slow: "text-candidate",
  bad: "text-down",
} as const;

export function NetworkPanel({ launcher }: { launcher: Launcher }) {
  const sliderId = useId();
  const { network, online, networkBusy, changeNetwork } = launcher;
  const available = online && network.enabled;

  // While dragging we show the draft; the request is sent once the slider settles.
  const [draft, setDraft] = useState<number | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const delay = draft ?? network.delayMs;
  const zone = delayZone(delay);

  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current);
    },
    [],
  );

  function slide(value: number) {
    setDraft(value);
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => {
      void changeNetwork({ kind: "delay", ms: value }).finally(() => setDraft(null));
    }, 250);
  }

  const hasFaults = network.delayMs > 0 || network.blocked.length > 0;

  const button =
    "min-h-11 rounded-lg border border-line-2 px-3 text-sm font-medium text-dim transition-colors enabled:hover:border-mute enabled:hover:text-fg disabled:cursor-not-allowed disabled:opacity-40 md:min-h-9";

  return (
    <Panel
      title="Network"
      aside={<span className="font-mono whitespace-nowrap">between nodes</span>}
    >
      {!available ? (
        <p className="mb-3 flex items-start gap-2 rounded-lg border border-line bg-cell px-3 py-2 text-xs text-mute">
          <i className="bx bx-info-circle mt-px text-sm" aria-hidden="true" />
          <span>
            {online
              ? "The launcher was started with --no-network, so delay and partitions are off."
              : "Launcher offline, so network controls are disabled."}
          </span>
        </p>
      ) : null}

      <div className="flex flex-col gap-3">
        <div>
          <label
            htmlFor={sliderId}
            className="flex items-baseline justify-between text-xs text-dim"
          >
            <span>Delay each way</span>
            <span className="font-mono text-sm whitespace-nowrap text-fg">{delay} ms</span>
          </label>
          <input
            id={sliderId}
            type="range"
            min={0}
            max={MAX_DELAY}
            step={10}
            value={delay}
            disabled={!available}
            onChange={(event) => slide(Number(event.target.value))}
            className="mt-1 h-11 w-full accent-leader disabled:opacity-40 md:h-7"
          />
          <p className={cn("font-mono text-[11px]", ZONE_TEXT[zone.level])}>{zone.label}</p>
        </div>

        <div className="grid grid-cols-2 gap-2">
          <button
            type="button"
            className={button}
            disabled={!available || networkBusy || network.blocked.length === 0}
            onClick={() => void changeNetwork({ kind: "heal" })}
          >
            Heal network
          </button>
          <button
            type="button"
            className={button}
            disabled={!available || networkBusy || !hasFaults}
            onClick={() => void changeNetwork({ kind: "reset" })}
          >
            Reset all
          </button>
        </div>

        {network.isolated.length > 0 ? (
          <p className="font-mono text-[11px] text-candidate">
            Cut off: {network.isolated.join(", ")}
          </p>
        ) : null}
      </div>
    </Panel>
  );
}
