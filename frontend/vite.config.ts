import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import wails from "@wailsio/runtime/plugins/vite";
import tailwindcss from "@tailwindcss/vite";
import path from "node:path";

// https://vitejs.dev/config/
export default defineConfig({
  server: {
    // 0.0.0.0 so docker's port publish can reach it; the compose mapping is
    // still bound to the host's loopback only.
    host: "0.0.0.0",
    port: Number(process.env.WAILS_VITE_PORT) || 9245,
    strictPort: true,
  },
  plugins: [react(), tailwindcss(), wails("./bindings")],
  resolve: {
    alias: { "@": path.resolve(__dirname, "./src") },
  },
});
