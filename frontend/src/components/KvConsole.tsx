import { useId, useState } from "react";
import { Panel } from "@/components/Panel";
import { describeStatus, kvRequest, type KvMethod, type KvResult } from "@/lib/kv";
import type { ClusterModel } from "@/lib/model";
import { cn } from "@/lib/utils";

interface Outcome {
  tone: "ok" | "bad" | "info";
  text: string;
  detail?: string;
}

const BURST = 20;

function summarize(method: KvMethod, key: string, result: KvResult, target: string): Outcome {
  const detail = `${describeStatus(result.status)} · ${result.ms} ms`;

  if (result.status === 0)
    return { tone: "bad", text: `Could not reach ${target}`, detail: result.body };
  if (method === "GET" && result.status === 200)
    return { tone: "ok", text: `${key} = ${result.body}`, detail };
  if (method === "GET" && result.status === 404)
    return { tone: "info", text: `${key} is not set`, detail };
  if (result.status === 204)
    return {
      tone: "ok",
      text: method === "PUT" ? `Stored ${key}` : `Deleted ${key}`,
      detail: `${detail} · committed by a majority`,
    };
  if (result.status === 503)
    return {
      tone: "bad",
      text: result.body || "Cluster unavailable",
      detail: `${detail} · try again`,
    };

  return { tone: "bad", text: result.body || "Request failed", detail };
}

export function KvConsole({ model }: { model: ClusterModel }) {
  const keyId = useId();
  const valueId = useId();
  const [key, setKey] = useState("greeting");
  const [value, setValue] = useState("hello world");
  const [busy, setBusy] = useState(false);
  const [outcome, setOutcome] = useState<Outcome | null>(null);

  // Talk to the leader directly (no redirect hop); else any node that is up and connected.
  const targetNode =
    model.nodes.find((node) => node.id === model.leader) ??
    model.nodes.find((node) => node.live && !node.isolated) ??
    null;
  const target = targetNode ? { id: targetNode.id, url: targetNode.ref.url } : null;

  const disabled = busy || !target;

  async function run(method: KvMethod) {
    if (!target) return;
    if (key.length === 0) {
      setOutcome({ tone: "bad", text: "Enter a key first" });
      return;
    }

    setBusy(true);
    const result = await kvRequest(target.url, method, key, value);
    setOutcome(summarize(method, key, result, target.id));
    setBusy(false);
  }

  async function burst() {
    if (!target) return;

    setBusy(true);
    const started = performance.now();

    for (let i = 1; i <= BURST; i += 1) {
      setOutcome({ tone: "info", text: `Writing demo-${i} of ${BURST}…` });
      const result = await kvRequest(target.url, "PUT", `demo-${i}`, `value ${i}`);

      if (!result.ok) {
        setOutcome(summarize("PUT", `demo-${i}`, result, target.id));
        setBusy(false);
        return;
      }
    }

    const ms = Math.round(performance.now() - started);
    setOutcome({
      tone: "ok",
      text: `Wrote ${BURST} keys`,
      detail: `${ms} ms · watch the log lanes fill`,
    });
    setBusy(false);
  }

  const field =
    "w-full rounded-lg border border-line-2 bg-ink px-3 py-2.5 font-mono text-base text-fg placeholder:text-mute md:text-[13px]";
  const action =
    "min-h-11 rounded-lg border border-line-2 px-3 text-sm font-medium text-dim transition-colors enabled:hover:border-mute enabled:hover:text-fg disabled:cursor-not-allowed disabled:opacity-40 md:min-h-9";

  return (
    <Panel
      title="Key-value console"
      aside={
        <span className="font-mono whitespace-nowrap">
          {target ? `via ${target.id}` : "no node reachable"}
        </span>
      }
    >
      <div className="flex flex-col gap-2.5">
        <label htmlFor={keyId} className="sr-only">
          Key
        </label>
        <input
          id={keyId}
          className={field}
          value={key}
          onChange={(event) => setKey(event.target.value)}
          placeholder="key"
          autoCapitalize="none"
          autoCorrect="off"
          spellCheck={false}
        />
        <label htmlFor={valueId} className="sr-only">
          Value
        </label>
        <input
          id={valueId}
          className={field}
          value={value}
          onChange={(event) => setValue(event.target.value)}
          placeholder="value"
          autoCapitalize="none"
          autoCorrect="off"
          spellCheck={false}
        />

        <div className="grid grid-cols-3 gap-2">
          <button
            type="button"
            disabled={disabled}
            onClick={() => void run("PUT")}
            className="min-h-11 rounded-lg bg-leader px-3 text-sm font-bold text-ink transition-opacity enabled:hover:opacity-90 disabled:cursor-not-allowed disabled:opacity-40 md:min-h-9"
          >
            PUT
          </button>
          <button
            type="button"
            disabled={disabled}
            onClick={() => void run("GET")}
            className={action}
          >
            GET
          </button>
          <button
            type="button"
            disabled={disabled}
            onClick={() => void run("DELETE")}
            className={action}
          >
            DELETE
          </button>
        </div>

        <button
          type="button"
          disabled={disabled}
          onClick={() => void burst()}
          className={cn(action, "flex items-center justify-center gap-2")}
        >
          <i className="bx bx-layer text-base" aria-hidden="true" />
          Write {BURST} keys
        </button>

        <div
          aria-live="polite"
          className={cn(
            "min-h-[3.25rem] rounded-lg border bg-ink px-3 py-2.5 font-mono text-xs break-words",
            outcome?.tone === "ok" && "border-ok/30 text-ok",
            outcome?.tone === "bad" && "border-down/40 text-down",
            outcome?.tone === "info" && "border-line-2 text-dim",
            !outcome && "border-line text-mute",
          )}
        >
          {outcome ? (
            <>
              <div>{outcome.text}</div>
              {outcome.detail ? <div className="mt-0.5 text-mute">{outcome.detail}</div> : null}
            </>
          ) : (
            "Results appear here."
          )}
        </div>
      </div>
    </Panel>
  );
}
