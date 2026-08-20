#!/usr/bin/env node

import { access, mkdtemp, rm } from 'node:fs/promises';
import { constants } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { once } from 'node:events';

const sleep = milliseconds =>
  new Promise(resolve => setTimeout(resolve, milliseconds));

async function availableExecutable() {
  const candidates = [
    process.env.GATHRA_CHROME,
    '/usr/bin/google-chrome',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ].filter(Boolean);
  for (const candidate of candidates) {
    try {
      await access(candidate, constants.X_OK);
      return candidate;
    } catch {
      // Try the next supported browser path.
    }
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

async function waitForTarget(port, dashboardUrl) {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    try {
      const targets = await fetch(`http://127.0.0.1:${port}/json/list`).then(
        response => response.json(),
      );
      const target = targets.find(
        candidate => candidate.type === 'page' &&
          candidate.url.startsWith(dashboardUrl),
      );
      if (target) return target;
    } catch {
      // Chrome may not have opened its debugging socket yet.
    }
    await sleep(200);
  }
  throw new Error('dashboard did not load in Chrome within 15 seconds');
}

class DevToolsClient {
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
    if (this.socket.readyState === WebSocket.OPEN) return;
    await new Promise((resolve, reject) => {
      this.socket.onopen = resolve;
      this.socket.onerror = reject;
    });
  }

  command(method, params = {}) {
    const id = this.nextId++;
    this.socket.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
    });
  }

  async evaluate(expression) {
    const response = await this.command('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
    });
    if (response.exceptionDetails) {
      throw new Error(response.exceptionDetails.exception?.description ||
        response.exceptionDetails.text);
    }
    return response.result.value;
  }

  close() {
    this.socket.close();
  }
}

async function main() {
  if (typeof WebSocket === 'undefined') {
    throw new Error('Node.js 22 or newer is required for this HIL test');
  }
  const dashboardUrl = new URL(process.argv[2] || 'http://192.168.4.1/');
  const executable = await availableExecutable();
  const profile = await mkdtemp(join(tmpdir(), 'gathra-dashboard-test-'));
  const port = await unusedPort();
  const chrome = spawn(executable, [
    '--headless=new',
    '--no-sandbox',
    '--disable-gpu',
    '--disable-background-networking',
    `--user-data-dir=${profile}`,
    `--remote-debugging-port=${port}`,
    '--remote-allow-origins=*',
    dashboardUrl.href,
  ], { stdio: 'ignore' });

  let client;
  try {
    const target = await waitForTarget(port, dashboardUrl.origin);
    client = new DevToolsClient(target.webSocketDebuggerUrl);
    await client.open();
    await client.command('Runtime.enable');

    const deadline = Date.now() + 10_000;
    while (Date.now() < deadline) {
      if (await client.evaluate(
        'document.readyState === "complete" && !!document.querySelector("#wifiForm")',
      )) break;
      await sleep(200);
    }
    await sleep(1_000);

    const initialSsid = await client.evaluate(
      'document.querySelector("#wifiForm").elements.ssid.value',
    );
    const edited = await client.evaluate(`(() => {
      const form = document.querySelector('#wifiForm');
      form.elements.ssid.focus();
      form.elements.ssid.value = 'DIAGNOSTIC-NEW-SSID';
      form.elements.ssid.dispatchEvent(new Event('input', {bubbles: true}));
      form.elements.password.focus();
      form.elements.password.value = 'not-submitted';
      form.elements.password.dispatchEvent(new Event('input', {bubbles: true}));
      return new URLSearchParams(new FormData(form)).get('ssid');
    })()`);

    // The production dashboard polls every five seconds. Waiting beyond that
    // interval reproduces the original credential overwrite without POSTing.
    await sleep(6_200);
    const afterPoll = await client.evaluate(`(() => {
      const form = document.querySelector('#wifiForm');
      const submitted = new URLSearchParams(new FormData(form));
      return {
        ssid: form.elements.ssid.value,
        password: form.elements.password.value,
        submittedSsid: submitted.get('ssid'),
        active: document.activeElement?.name || '',
      };
    })()`);

    if (edited !== 'DIAGNOSTIC-NEW-SSID' ||
        afterPoll.ssid !== 'DIAGNOSTIC-NEW-SSID' ||
        afterPoll.submittedSsid !== 'DIAGNOSTIC-NEW-SSID' ||
        afterPoll.password !== 'not-submitted') {
      throw new Error(`unsaved form values were overwritten: ${JSON.stringify({
        initialSsid,
        edited,
        afterPoll,
      })}`);
    }
    console.log(JSON.stringify({
      status: 'PASS',
      initialSsid,
      afterPoll,
      submitted: false,
    }, null, 2));
  } finally {
    client?.close();
    chrome.kill('SIGTERM');
    await Promise.race([once(chrome, 'exit'), sleep(2_000)]);
    if (chrome.exitCode === null) chrome.kill('SIGKILL');
    await rm(profile, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error.stack || error.message);
  process.exitCode = 1;
});
