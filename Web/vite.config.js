import fs from 'fs/promises';
import { defineConfig } from 'vite';
import path from 'path';
import react from '@vitejs/plugin-react';
import { NodeGlobalsPolyfillPlugin } from '@esbuild-plugins/node-globals-polyfill';
import { nodePolyfills } from 'vite-plugin-node-polyfills';

export default defineConfig(() => ({
  server: {
    port: 3000,
  },
  plugins: [
    react(),
    nodePolyfills(),
  ],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, './src'),
      tinyqueue: path.join(__dirname, 'node_modules', 'tinyqueue', 'index.js'),
    },
  },
  build: {
    outDir: 'build',
    target: 'esnext',
    rollupOptions: {
      // Optional: include .js as jsx in build too
      plugins: [],
    },
  },
  define: {
    'process.env': process.env ?? {},
  },
  esbuild: {
    loader: 'jsx',
    include: /\.js$/, // Match all .js files
    exclude: /node_modules/, // Exclude node_modules
  },
  optimizeDeps: {
    esbuildOptions: {
      plugins: [
        NodeGlobalsPolyfillPlugin({ buffer: false, process: true }),
        {
          name: 'load-js-files-as-jsx',
          setup(build) {
            build.onLoad({ filter: /\.js$/ }, async (args) => {
              const contents = await fs.readFile(args.path, 'utf8');
              return {
                loader: 'jsx',
                contents,
              };
            });
          },
        },
      ],
    },
  },
}));
