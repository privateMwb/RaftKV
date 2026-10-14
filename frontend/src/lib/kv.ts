// Client for the backend's `/kv/:key` API. Values are raw text bodies, which is why
// this doesn't use lib/api.ts (that helper speaks JSON in both directions).

export type KvMethod = "GET" | "PUT" | "DELETE";

export interface KvResult {
  ok: boolean;
  /** HTTP status, or 0 when the request never got an answer. */
  status: number;
  /** The value for a successful GET; the server's message for an error. */
  body: string;
  ms: number;
}

export interface KvOptions {
  fetchFn?: typeof fetch;
  timeoutMs?: number;
}

export async function kvRequest(
  baseUrl: string,
  method: KvMethod,
  key: string,
  value?: string,
  options: KvOptions = {},
): Promise<KvResult> {
  const { fetchFn = (...args) => fetch(...args), timeoutMs = 5000 } = options;
  const started = performance.now();

  try {
    const response = await fetchFn(`${baseUrl}/kv/${encodeURIComponent(key)}`, {
      method,
      body: method === "PUT" ? (value ?? "") : undefined,
      signal: AbortSignal.timeout(timeoutMs),
    });

    return {
      ok: response.ok,
      status: response.status,
      body: response.status === 204 ? "" : await response.text(),
      ms: Math.round(performance.now() - started),
    };
  } catch (error) {
    const timedOut = error instanceof DOMException && error.name === "TimeoutError";
    return {
      ok: false,
      status: 0,
      body: timedOut ? "timed out" : "could not reach the node",
      ms: Math.round(performance.now() - started),
    };
  }
}

const STATUS_TEXT: Record<number, string> = {
  200: "OK",
  204: "No Content",
  307: "Temporary Redirect",
  400: "Bad Request",
  404: "Not Found",
  413: "Payload Too Large",
  429: "Too Many Requests",
  500: "Internal Server Error",
  503: "Service Unavailable",
};

/** "204 No Content", or "no answer" for status 0. */
export function describeStatus(status: number): string {
  if (status === 0) return "no answer";
  return STATUS_TEXT[status] ? `${status} ${STATUS_TEXT[status]}` : String(status);
}
