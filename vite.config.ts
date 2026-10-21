import { defineConfig, type Plugin } from 'vite';
import react from '@vitejs/plugin-react';
import { version } from './package.json';

// Strict CSP for the packaged app only; the dev server needs React Refresh's inline preamble.
const csp: Plugin = {
  name: 'prism-csp',
  apply: 'build',
  transformIndexHtml: (html) =>
    html.replace(
      '<meta charset="UTF-8" />',
      `<meta charset="UTF-8" />\n    <meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; font-src 'self' data:; img-src 'self' data:" />`,
    ),
};

export default defineConfig({
  plugins: [react(), csp],
  define: { __APP_VERSION__: JSON.stringify(version) },
  base: './',
  server: { port: 5173, strictPort: true },
  build: { outDir: 'dist', emptyOutDir: true, target: 'chrome128' },
});
