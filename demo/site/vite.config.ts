/// <reference types="vitest/config" />
import { defineConfig } from "vite";

// Relative base so the built site works when served from a GitHub Pages project subpath
// (hosting itself is PLAN Stage 5 step 5's job, not this lane's -- base just needs to not
// assume it is served from the domain root).
export default defineConfig({
  base: "./",
  test: {
    environment: "node",
  },
});
