// A small incremental Server-Sent Events parser. We read the stream with fetch()
// rather than EventSource so reconnects and failover to another node stay under
// our control (EventSource can only retry the one URL it was given).

export interface SseMessage {
  event: string;
  data: string;
}

export class SseParser {
  private buffer = "";

  /** Feeds a chunk of text and returns every message it completed. */
  feed(chunk: string): SseMessage[] {
    this.buffer += chunk.replace(/\r\n?/g, "\n");

    const messages: SseMessage[] = [];
    let end = this.buffer.indexOf("\n\n");

    while (end !== -1) {
      const block = this.buffer.slice(0, end);
      this.buffer = this.buffer.slice(end + 2);

      const message = parseBlock(block);
      if (message) messages.push(message);

      end = this.buffer.indexOf("\n\n");
    }

    return messages;
  }
}

function parseBlock(block: string): SseMessage | null {
  let event = "message";
  const data: string[] = [];
  let sawField = false;

  for (const line of block.split("\n")) {
    if (line === "" || line.startsWith(":")) continue; // blank or comment

    const colon = line.indexOf(":");
    const field = colon === -1 ? line : line.slice(0, colon);
    let value = colon === -1 ? "" : line.slice(colon + 1);
    if (value.startsWith(" ")) value = value.slice(1);

    if (field === "event") {
      event = value;
      sawField = true;
    } else if (field === "data") {
      data.push(value);
      sawField = true;
    }
  }

  return sawField ? { event, data: data.join("\n") } : null;
}
