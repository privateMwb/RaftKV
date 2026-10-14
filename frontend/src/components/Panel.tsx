import type { ReactNode } from "react";
import { cn } from "@/lib/utils";

interface PanelProps {
  title: string;
  /** Small content on the right of the title row (legend, current target, ...). */
  aside?: ReactNode;
  className?: string;
  children: ReactNode;
}

export function Panel({ title, aside, className, children }: PanelProps) {
  return (
    <section className={cn("flex flex-col rounded-2xl border border-line bg-panel p-4", className)}>
      <header className="mb-3 flex items-center justify-between gap-3">
        <h2 className="text-[11px] font-semibold tracking-[0.1em] whitespace-nowrap text-mute uppercase">
          {title}
        </h2>
        {aside ? <div className="min-w-0 text-xs text-mute">{aside}</div> : null}
      </header>
      {children}
    </section>
  );
}
