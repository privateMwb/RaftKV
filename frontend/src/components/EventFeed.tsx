import { Panel } from "@/components/Panel";
import { formatClock } from "@/lib/format";
import type { FeedEvent, FeedTone } from "@/lib/types";
import { cn } from "@/lib/utils";

const TONE: Record<FeedTone, string> = {
  info: "text-dim",
  good: "text-ok",
  warn: "text-candidate",
  bad: "text-down",
  leader: "text-leader",
};

export function EventFeed({ events }: { events: FeedEvent[] }) {
  return (
    <Panel title="Events" aside={<span className="font-mono">live</span>} className="min-h-0">
      {events.length === 0 ? (
        <p className="font-mono text-xs text-mute">
          Nothing yet. Kill a node to see the cluster react.
        </p>
      ) : (
        <ol className="max-h-72 overflow-y-auto lg:max-h-[26rem]" aria-live="polite">
          {events.map((event) => (
            <li
              key={event.id}
              className="flex gap-3 border-t border-line py-2 font-mono text-xs first:border-t-0"
            >
              <time className="shrink-0 text-mute">{formatClock(event.at)}</time>
              <span className={cn("min-w-0 break-words", TONE[event.tone])}>{event.text}</span>
            </li>
          ))}
        </ol>
      )}
    </Panel>
  );
}
