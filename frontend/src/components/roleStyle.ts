import type { Role } from "@/lib/types";

/** What a node looks like on screen: its role while up, or down / unknown. */
export type Display = Role | "down" | "unknown";

interface Style {
  label: string;
  /** Coloured text. */
  text: string;
  /** Badge background + text. */
  badge: string;
  /** Status dot. */
  dot: string;
  /** Card border. */
  border: string;
}

export const ROLE_STYLE: Record<Display, Style> = {
  leader: {
    label: "LEADER",
    text: "text-leader",
    badge: "bg-leader/15 text-leader",
    dot: "bg-leader",
    border: "border-leader shadow-[0_0_28px_-6px_var(--color-leader)]",
  },
  follower: {
    label: "FOLLOWER",
    text: "text-follower",
    badge: "bg-follower/15 text-follower",
    dot: "bg-follower",
    border: "border-line-2",
  },
  candidate: {
    label: "CANDIDATE",
    text: "text-candidate",
    badge: "bg-candidate/15 text-candidate",
    dot: "bg-candidate",
    border: "border-candidate/70",
  },
  down: {
    label: "DOWN",
    text: "text-down",
    badge: "bg-down/15 text-down",
    dot: "bg-down",
    border: "border-dashed border-down/50 opacity-90",
  },
  unknown: {
    label: "…",
    text: "text-mute",
    badge: "bg-line text-mute",
    dot: "bg-mute",
    border: "border-line-2 opacity-70",
  },
};
