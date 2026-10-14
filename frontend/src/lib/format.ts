/** "12:04:09" in the viewer's local time. */
export function formatClock(at: number): string {
  const date = new Date(at);
  return [date.getHours(), date.getMinutes(), date.getSeconds()]
    .map((part) => String(part).padStart(2, "0"))
    .join(":");
}

/** The `:port` of a node URL, for display ("" if it can't be parsed). */
export function portOf(url: string): string {
  try {
    const { port } = new URL(url);
    return port ? `:${port}` : "";
  } catch {
    return "";
  }
}
