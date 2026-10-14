/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_API_BASE_URL: string;
  /** Host the nodes run on; defaults to the host that served this page. */
  readonly VITE_NODE_HOST?: string;
  /** Comma-separated node ports, in node-1, node-2, ... order. Default 8101,8102,8103. */
  readonly VITE_NODE_PORTS?: string;
  /** Launcher base URL; defaults to http://<node host>:8099. */
  readonly VITE_LAUNCHER_URL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
