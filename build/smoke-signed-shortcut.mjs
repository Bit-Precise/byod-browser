// Validate only an isolated browser launched through a test .lnk, never the
// installed browser/profile. Usage: node smoke-signed-shortcut.mjs CDP_PORT VERSION
import assert from 'node:assert/strict';
const port = Number(process.argv[2]);
const version = process.argv[3];
assert(Number.isInteger(port) && port > 0 && port < 65536);
assert(/^\d+\.\d+\.\d+\.\d+$/.test(version || ''));
const base = `http://127.0.0.1:${port}`;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let pages;
for (let i = 0; i < 60; i++) {
  try {
    pages = (await (await fetch(base + '/json/list', {signal: AbortSignal.timeout(1000)})).json())
      .filter(t => t.type === 'page');
    if (pages.length) break;
  } catch {}
  await delay(500);
}
assert.equal(pages?.length, 1, 'shortcut must open exactly one page');
assert.equal(pages[0].url, 'about:blank', 'spaces in arguments created a bogus tab');
const socket = new WebSocket(pages[0].webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener('open', resolve, {once: true});
  socket.addEventListener('error', reject, {once: true});
});
let id = 0;
const pending = new Map();
socket.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  const entry = pending.get(message.id);
  if (!entry) return;
  pending.delete(message.id);
  clearTimeout(entry.timer);
  message.error ? entry.reject(Error(JSON.stringify(message.error))) : entry.resolve(message.result);
});
function call(method, params = {}) {
  return new Promise((resolve, reject) => {
    const key = ++id;
    const timer = setTimeout(() => { pending.delete(key); reject(Error('timeout ' + method)); }, 15000);
    pending.set(key, {resolve, reject, timer});
    socket.send(JSON.stringify({id: key, method, params}));
  });
}
async function evaluate(expression) {
  const result = await call('Runtime.evaluate', {expression, returnByValue: true});
  assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
  return result.result.value;
}
try {
  await call('Page.enable');
  const info = await call('Browser.getVersion');
  assert(info.product.endsWith('/' + version));
  assert.equal(info.userAgent.split(' ').filter(x => x === 'BYODBrowser/' + version).length, 1);
  assert.equal(await evaluate('navigator.userAgent'), info.userAgent);
  await call('Page.navigate', {url: 'chrome://newtab/'});
  let ready = false;
  for (let i = 0; i < 60 && !ready; i++) {
    try { ready = await evaluate("location.hostname === 'new-tab-page-third-party' && document.readyState === 'complete'"); } catch {}
    if (!ready) await delay(250);
  }
  assert(ready, 'signed local new-tab page did not load');
  console.log(JSON.stringify({signedShortcut: 'PASS', singleTab: true, localNewTab: true, version, userAgent: info.userAgent}));
} finally {
  try { await call('Browser.close'); } catch {}
  socket.close();
  for (const entry of pending.values()) clearTimeout(entry.timer);
}
