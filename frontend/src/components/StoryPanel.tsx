import { Panel } from "@/components/Panel";
import type { ClusterModel } from "@/lib/model";
import { explain, type StoryTone } from "@/lib/story";
import { cn } from "@/lib/utils";

const TONE: Record<StoryTone, { icon: string; text: string }> = {
  good: { icon: "bx-check-circle", text: "text-ok" },
  info: { icon: "bx-info-circle", text: "text-dim" },
  warn: { icon: "bx-error-circle", text: "text-candidate" },
  bad: { icon: "bx-error-circle", text: "text-down" },
};

const TRY = [
  "Write 20 keys and watch the log lanes fill, then commit on every node.",
  "Kill the leader. An election runs, the term goes up, and the dashboard keeps working.",
  "Isolate the leader. It stays alive but stale while the other two elect a new leader. Try a write on each.",
  "Heal the network. The stale leader steps down and its uncommitted entries disappear.",
  "Set the delay to 80 ms: heartbeats now arrive slower than the election timeout and leadership flaps.",
  "Kill two nodes. With no majority the cluster refuses to answer rather than guess.",
];

export function StoryPanel({ model, delayMs }: { model: ClusterModel; delayMs: number }) {
  const story = explain(model, delayMs);
  const look = TONE[story.tone];

  return (
    <Panel title="What's happening">
      <div aria-live="polite" className="flex gap-3">
        <i className={cn("bx mt-0.5 text-xl", look.icon, look.text)} aria-hidden="true" />
        <div className="min-w-0">
          <h3 className="font-semibold">{story.title}</h3>
          <p className="mt-1 text-sm leading-relaxed text-dim">{story.body}</p>
          {story.hint ? (
            <p className="mt-2 flex items-start gap-1.5 text-xs text-mute">
              <i className="bx bx-bulb mt-px text-sm" aria-hidden="true" />
              <span>{story.hint}</span>
            </p>
          ) : null}
          {story.note ? (
            <p className="mt-3 rounded-lg border border-candidate/30 bg-candidate/5 px-3 py-2 text-xs leading-relaxed text-candidate">
              {story.note}
            </p>
          ) : null}
        </div>
      </div>

      <details className="mt-4 border-t border-line pt-1">
        <summary className="flex min-h-11 cursor-pointer items-center text-[11px] font-semibold tracking-[0.1em] text-mute uppercase">
          Things to try
        </summary>
        <ol className="mt-1 list-decimal space-y-2 pl-5 text-xs leading-relaxed text-dim">
          {TRY.map((line) => (
            <li key={line}>{line}</li>
          ))}
        </ol>
      </details>
    </Panel>
  );
}
