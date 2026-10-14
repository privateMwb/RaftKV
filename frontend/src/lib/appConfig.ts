import { buildConfig } from "./config.ts";

/** The runtime configuration: Vite env overrides, else "same host as this page". */
export const appConfig = buildConfig(import.meta.env, window.location.hostname);
