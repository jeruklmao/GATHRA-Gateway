#!/usr/bin/env node

import { access, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { constants } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { once } from 'node:events';

const sleep = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

async function browserExecutable() {
  for (const candidate of [process.env.GATHRA_CHROME, '/usr/bin/google-chrome',
    '/usr/bin/chromium', '/usr/bin/chromium-browser'].filter(Boolean)) {
    try { await access(candidate, constants.X_OK); return candidate; } catch {}
  }
  throw new Error('Chrome/Chromium not found; set GATHRA_CHROME');
}

async function unusedPort() {
  const server = createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { port } = server.address();
  server.close();
  await once(server, 'close');
  return port;
}

class DevTools {
  constructor(url) {
    this.socket = new WebSocket(url);
    this.nextId = 1;
    this.pending = new Map();
    this.socket.onmessage = event => {
      const message = JSON.parse(event.data);
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) pending.reject(new Error(JSON.stringify(message.error)));
      else pending.resolve(message.result);
    };
  }
  async open() {
    if (this.socket.readyState !== WebSocket.OPEN) {
      await new Promise((resolve, reject) => {
        this.socket.onopen = resolve;
        this.socket.onerror = reject;
      });
    }
  }
  command(method, params = {}) {
    const id = this.nextId++;
    this.socket.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
  async evaluate(expression) {
    const response = await this.command('Runtime.evaluate', {
      expression, returnByValue: true, awaitPromise: true,
    });
    if (response.exceptionDetails) throw new Error(response.exceptionDetails.text);
    return response.result.value;
  }
  close() { this.socket.close(); }
}

async function main() {
  if (typeof WebSocket === 'undefined') throw new Error('Node.js 22 or newer is required');
  const source = await readFile(new URL('../lib/dashboard/dashboard_html.hpp', import.meta.url), 'utf8');
  const match = source.match(/R"GTHHTML\(([\s\S]*?)\)GTHHTML"/);
  if (!match) throw new Error('embedded dashboard HTML was not found');
  const directory = await mkdtemp(join(tmpdir(), 'gathra-dashboard-layout-'));
  const htmlPath = join(directory, 'index.html');
  await writeFile(htmlPath, match[1]);
  const url = pathToFileURL(htmlPath).href;
  const executable = await browserExecutable();
  const port = await unusedPort();
  const chrome = spawn(executable, ['--headless=new', '--no-sandbox', '--disable-gpu',
    `--user-data-dir=${join(directory, 'profile')}`, `--remote-debugging-port=${port}`,
    '--remote-allow-origins=*', url], { stdio: 'ignore' });
  let client;
  try {
    let target;
    for (let attempt = 0; attempt < 50 && !target; ++attempt) {
      try {
        const targets = await fetch(`http://127.0.0.1:${port}/json/list`).then(r => r.json());
        target = targets.find(item => item.type === 'page' && item.url === url);
      } catch {}
      if (!target) await sleep(100);
    }
    if (!target) throw new Error('headless dashboard target did not open');
    client = new DevTools(target.webSocketDebuggerUrl);
    await client.open();
    await client.command('Runtime.enable');
    const results = [];
    for (const viewport of [{ name: 'desktop', width: 1440, height: 900 },
      { name: 'tablet', width: 800, height: 900 },
      { name: 'mobile', width: 360, height: 800 }]) {
      await client.command('Emulation.setDeviceMetricsOverride', {
        width: viewport.width, height: viewport.height, deviceScaleFactor: 1, mobile: false,
      });
      await sleep(100);
      const layout = await client.evaluate(`(() => {
        const selectors = ['main','.card','form','input','button','.kv'];
        const offenders = [...document.querySelectorAll(selectors.join(','))]
          .filter(e => { const r=e.getBoundingClientRect(); return r.left < -1 || r.right > innerWidth + 1 || e.scrollWidth > e.clientWidth + 1; })
          .map(e => e.id || e.tagName + '.' + e.className).slice(0,10);
        return {viewport:innerWidth,documentWidth:document.documentElement.scrollWidth,
          bodyWidth:document.body.scrollWidth,offenders};
      })()`);
      if (layout.documentWidth > viewport.width + 1 ||
          layout.bodyWidth > viewport.width + 1 || layout.offenders.length) {
        throw new Error(`${viewport.name} overflow: ${JSON.stringify(layout)}`);
      }
      results.push({ name: viewport.name, width: viewport.width, status: 'PASS' });
    }
    console.log(JSON.stringify(results, null, 2));
  } finally {
    client?.close();
    chrome.kill('SIGTERM');
    await Promise.race([once(chrome, 'exit'), sleep(2000)]);
    if (chrome.exitCode === null) chrome.kill('SIGKILL');
    await rm(directory, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error.stack || error.message); process.exitCode = 1; });
