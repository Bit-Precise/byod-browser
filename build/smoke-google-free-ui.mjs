// Run only against a new, isolated browser launched for this test.
// Usage: node smoke-google-free-ui.mjs CDP_PORT EXPECTED_VERSION [FIXTURE_URL]
// Optional HTTPS fixture: map www.google.com to the isolated fixture using
// browser-only host rules/SPKI. Do not change system DNS or certificate trust.
// Checks local UI/UA only; this is not a complete Google-network traffic audit.
import assert from 'node:assert/strict';

const port = Number(process.argv[2]);
const version = process.argv[3];
const fixtureURL = process.argv[4] ? new URL(process.argv[4]) : null;
assert(Number.isInteger(port) && port > 0 && port < 65536);
assert(/^\d+\.\d+\.\d+\.\d+$/.test(version || ''));
if (fixtureURL) {
  assert.equal(fixtureURL.protocol, 'https:');
  assert.equal(fixtureURL.hostname, 'www.google.com');
  assert(!fixtureURL.username && !fixtureURL.password);
}
const cdp = `http://127.0.0.1:${port}`;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let target;
for (let i = 0; i < 60 && !target; i++) {
  try {
    const targets = await (await fetch(cdp + '/json/list', {
      signal: AbortSignal.timeout(1000),
    })).json();
    target = targets.find(t => t.type === 'page');
  } catch {}
  if (!target) await delay(500);
}
assert(target, 'isolated browser page not found');
const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener('open', resolve, {once: true});
  socket.addEventListener('error', reject, {once: true});
});
let nextID = 0;
const pending = new Map();
socket.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  const entry = pending.get(message.id);
  if (!entry) return;
  pending.delete(message.id);
  clearTimeout(entry.timer);
  message.error ? entry.reject(Error(JSON.stringify(message.error))) :
    entry.resolve(message.result);
});
function call(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = ++nextID;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(Error('timeout: ' + method));
    }, 15000);
    pending.set(id, {resolve, reject, timer});
    socket.send(JSON.stringify({id, method, params}));
  });
}
async function evaluate(expression) {
  const result = await call('Runtime.evaluate', {
    expression, returnByValue: true, awaitPromise: true,
  });
  assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
  return result.result.value;
}
async function waitFor(expression, description) {
  for (let i = 0; i < 60; i++) {
    try { if (await evaluate(expression)) return; } catch {}
    await delay(250);
  }
  throw Error('timeout: ' + description);
}
const inspectProfile = `(() => {
  const elements = [];
  const visit = root => {
    for (const e of root.querySelectorAll('*')) {
      elements.push(e);
      if (e.shadowRoot) visit(e.shadowRoot);
    }
  };
  visit(document);
  const connect = elements.find(e => e.id === 'byod-connect-login')
    ?.querySelector('cr-button');
  const removed = elements.filter(e => e.matches(
    '#sync-setup, #google-services, #manage-google-account, ' +
    'settings-sync-account-control, settings-sync-page, ' +
    'settings-sync-controls-page, settings-google-services-page'));
  return {path: location.pathname, connect: !!connect,
    visible: !!connect?.getClientRects().length,
    googleElements: removed.map(e => e.localName + '#' + e.id)};
})()`;

try {
  await call('Page.enable');
  await call('Runtime.enable');
  const info = await call('Browser.getVersion');
  assert.equal(info.userAgent.split(' ').filter(x => x === 'BYODBrowser/' + version).length, 1);
  assert(info.product.endsWith('/' + version), info.product);
  const newTab = await call('Page.navigate', {url: 'chrome://newtab/'});
  assert(!newTab.errorText, newTab.errorText);
  await waitFor("location.protocol === 'chrome:' && " +
    "location.hostname === 'new-tab-page-third-party' && document.readyState === 'complete'",
    'local third-party new-tab page');
  const routes = ['/people', '/syncSetup', '/syncSetup/advanced', '/googleServices'];
  const results = [];
  for (const path of routes) {
    const result = await call('Page.navigate', {url: 'chrome://settings' + path});
    assert(!result.errorText, result.errorText);
    await waitFor(`(${inspectProfile}).connect && (${inspectProfile}).visible`,
      'Connect profile page after ' + path);
    const state = await evaluate(inspectProfile);
    assert.equal(state.path, '/people', path + ' did not return to local profile page');
    assert.deepEqual(state.googleElements, [], 'removed Google UI is still mounted');
    results.push(path);
  }
  // Exercise the real browser-process navigation, not only button presence.
  assert(await evaluate(`(() => {
    const visit = root => {
      const row = root.querySelector('#byod-connect-login');
      if (row) { row.querySelector('cr-button').click(); return true; }
      return [...root.querySelectorAll('*')].some(e => e.shadowRoot && visit(e.shadowRoot));
    };
    return visit(document);
  })()`), 'Connect button was not found for clicking');
  await waitFor("(location.protocol === 'grips:' && location.hostname === 'login') || " +
    "(location.protocol === 'https:' && location.hostname === 'connect.cs.ac.cn')",
    'Connect button native navigation');
  if (fixtureURL) {
    const nav = await call('Page.navigate', {url: fixtureURL.href});
    assert(!nav.errorText, nav.errorText);
    await waitFor("location.hostname === 'www.google.com' && " +
      "document.title === 'BYOD isolated control fixture'", 'ordinary website fixture');
    const probe = await evaluate(`(async () => {
      const response = await fetch('/byod-web-probe', {cache: 'no-store'});
      return {ok: response.ok, body: await response.text(), userAgent: navigator.userAgent};
    })()`);
    assert(probe.ok && probe.body.includes('BYOD isolated control fixture'),
      'ordinary web fetch failed or did not reach the loopback fixture');
    assert.equal(probe.userAgent.split(' ').filter(x => x === 'BYODBrowser/' + version).length, 1);
    // Let the fresh profile perform startup work without background-networking
    // suppression flags. NetLog is checked separately after Browser.close.
    await delay(10000);
  }
  await call('Page.navigate', {url: 'grips://exam.cs.ac.cn/'});
  await waitFor("location.protocol === 'grips:' && typeof chrome.send === 'function'",
    'native exam bootstrap bridge');
  console.log(JSON.stringify({localUI: 'PASS', version, userAgent: info.userAgent,
    localNewTab: 'PASS', connectEntry: 'PASS', connectButtonNavigation: 'PASS',
    removedGoogleSettings: 'PASS', redirects: results,
    gripsBridge: 'PASS', ordinaryGoogleHostNavigationAndFetch: fixtureURL ? 'PASS' : 'not-run',
    networkRemovalAudit: 'check completed NetLog separately'}));
} finally {
  try { await call('Browser.close'); } catch {}
  socket.close();
  for (const entry of pending.values()) clearTimeout(entry.timer);
}
