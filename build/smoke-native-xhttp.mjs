// Usage: node smoke-native-xhttp.mjs <private-fixture.json> <CDP-port> <outer-port>
//        [expected-protocol] [--without-audit] [--check-invalid-audit]
// Only connects to an isolated browser launched for this fixture.
import {readFileSync} from 'node:fs';
import {get} from 'node:https';
import assert from 'node:assert/strict';

const fixture = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const cdp = `http://127.0.0.1:${Number(process.argv[3])}`;
const outerPort = Number(process.argv[4]);
const withoutAudit = process.argv.includes('--without-audit');
const checkInvalidAudit = process.argv.includes('--check-invalid-audit');
const checkFullscreen = process.argv.includes('--check-fullscreen');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function stats() {
  return new Promise((resolve, reject) => {
    const request = get(fixture.front + '/fixture/stats', {
      ca: fixture.certificate, headers: {Authorization: 'Bearer ' + fixture.ticket},
    }, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => {body += chunk;});
      response.on('error', reject);
      response.on('end', () => {
        try {assert.equal(response.statusCode, 200); resolve(JSON.parse(body));} catch (error) {reject(error);}
      });
    });
    request.on('error', reject);
  });
}
const before = await stats();
let target;
for (let attempt = 0; attempt < 60 && !target; attempt++) {
  try {
    const targets = await (await fetch(`${cdp}/json/list`, {signal: AbortSignal.timeout(1000)})).json();
    target = targets.find(t => t.type === 'page' && t.url.startsWith('grips://exam.cs.ac.cn/'));
  } catch {}
  if (!target) await delay(500);
}
assert(target, 'isolated grips page not found');
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  ws.addEventListener('open', resolve, {once: true});
  ws.addEventListener('error', reject, {once: true});
});
let nextID = 1;
const pending = new Map();
ws.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  const entry = pending.get(message.id);
  if (!entry) return;
  pending.delete(message.id);
  clearTimeout(entry.timer);
  message.error ? entry.reject(new Error(JSON.stringify(message.error))) : entry.resolve(message.result);
});
function call(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = nextID++;
    const timer = setTimeout(() => {pending.delete(id); reject(new Error(`timeout: ${method}`));}, 45000);
    pending.set(id, {resolve, reject, timer});
    ws.send(JSON.stringify({id, method, params}));
  });
}
async function evaluate(expression) {
  const result = await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
  assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
  return result.result.value;
}
try {
  await call('Page.enable');
  await call('Runtime.enable');
  assert.equal(await evaluate('typeof chrome.send'), 'function');
  if (checkFullscreen) {
    const {windowId, bounds} = await call('Browser.getWindowForTarget', {targetId: target.id});
    assert.notEqual(bounds.windowState, 'fullscreen', 'fixture must start outside fullscreen');
    const state = async () => (await call('Browser.getWindowBounds', {windowId})).bounds.windowState;
    await evaluate("chrome.send('setByodFullscreen', [true, true]); true");
    for (let i = 0; i < 40 && await state() !== 'fullscreen'; i++) await delay(100);
    assert.equal(await state(), 'fullscreen', 'native fullscreen was not entered');
    await call('Input.dispatchKeyEvent', {type: 'rawKeyDown', key: 'F11', code: 'F11', windowsVirtualKeyCode: 122});
    await call('Input.dispatchKeyEvent', {type: 'keyUp', key: 'F11', code: 'F11', windowsVirtualKeyCode: 122});
    await delay(500);
    assert.equal(await state(), 'fullscreen', 'F11 escaped the native fullscreen lock');
    await evaluate("chrome.send('setByodFullscreen', [false, false]); true");
    for (let i = 0; i < 40 && await state() === 'fullscreen'; i++) await delay(100);
    assert.notEqual(await state(), 'fullscreen', 'native fullscreen was not restored');
  }
  // Bypass only the UI bootstrap, not the native transport or source TLS.
  const config = {sourceHost: '127.0.0.1', tunnelHosts: ['127.0.0.1'],
    proxyHost: 'exam.cs.ac.cn', proxyPort: outerPort, endpointId: 'course-101',
    ticket: fixture.ticket, allowedOrigins: [fixture.source], examEndsAtMs: 0,
    auditSessionId: fixture.audit_session_id, auditToken: fixture.audit_token};
  if (withoutAudit) {
    // Old deployed control planes omit both keys. Native transport and exam
    // lifecycle must still work, but must not invent audit credentials.
    delete config.auditSessionId;
    delete config.auditToken;
  }
  await evaluate(`chrome.send('setByodTunnelConfig', [${JSON.stringify(config)}]); true`);
  await delay(250);
  if (checkInvalidAudit) {
    // Each malformed bootstrap tries to clear routing. All must be rejected
    // before mutating the previously valid native configuration.
    const incomplete = {...config, tunnelHosts: [],
      auditSessionId: 'f8edc0b6-ef90-4a86-88a3-294bdc9ff000'};
    delete incomplete.auditToken;
    const wrongType = {...config, tunnelHosts: [], auditSessionId: 17,
      auditToken: 'not-a-real-token'};
    for (const invalid of [incomplete, wrongType]) {
      await evaluate(`chrome.send('setByodTunnelConfig', [${JSON.stringify(invalid)}]); true`);
      await delay(250);
    }
  }
  const nav = await call('Page.navigate', {url: fixture.source + '/native-smoke'});
  assert(!nav.errorText, nav.errorText);
  let title;
  for (let i = 0; i < 80; i++) {
    try { title = await evaluate('document.title'); } catch {}
    if (title === 'BYOD native XHTTP OK') break;
    await delay(250);
  }
  assert.equal(title, 'BYOD native XHTTP OK');
  const echo = await evaluate(`(async () => {
    const body = 'native-xhttp-\\u0000-'.repeat(16384);
    const results = await Promise.all(Array.from({length: 12}, async (_, i) => {
      const response = await fetch('/echo?request=' + i, {method: 'POST', body});
      const received = await response.text();
      if (!response.ok || received !== body)
        throw Error('echo mismatch ' + i + ': status=' + response.status +
          ' sent=' + body.length + ' received=' + received.length +
          ' prefix=' + JSON.stringify(received.slice(0, 100)));
      return body.length;
    }));
    return {requests: results.length, bytes: results.reduce((a, b) => a + b, 0)};
  })()`);
  assert.equal(echo.requests, 12);
  const during = await stats();
  assert(during.downloads > before.downloads && during.uploads > before.uploads,
    'source request did not traverse XHTTP');
  const expectedProtocol = process.argv[5]?.startsWith('--') ? undefined : process.argv[5];
  if (expectedProtocol)
    assert((during.protocols[expectedProtocol] || 0) > (before.protocols[expectedProtocol] || 0),
      'expected outer protocol not observed: ' + expectedProtocol);
  let sourceTLS = 'not-tested';
  if (fixture.bad_source) {
    const bad = await call('Page.navigate', {url: fixture.bad_source + '/untrusted'});
    assert.equal(bad.errorText, 'net::ERR_CERT_AUTHORITY_INVALID',
      'untrusted inner source must fail certificate validation');
    const rejected = await stats();
    assert.equal(rejected.bad_inner, 0, 'HTTP reached an untrusted TLS origin');
    assert(rejected.downloads > during.downloads, 'TLS failure did not traverse XHTTP');
    sourceTLS = 'untrusted-source-rejected';
  }
  // Clearing native policy must cancel active streams and remove routing.
  await call('Page.navigate', {url: 'grips://exam.cs.ac.cn/'});
  await delay(1500);
  await evaluate("chrome.send('clearByodTunnelConfig', []); true");
  await delay(500);
  const after = await stats();
  assert.equal(after.active, 0, 'clearing native policy left active streams');
  if (withoutAudit && Array.isArray(before.events)) {
    assert.equal(after.events.length, before.events.length,
      'bootstrap without audit credentials emitted audit events');
  }
  console.log(JSON.stringify({nativeTransport: 'PASS', page: title, echo,
    outerProtocols: during.protocols, nativeClear: 'PASS', activeAfterClear: after.active,
    auditBootstrap: withoutAudit ? 'omitted' : 'fixture',
    sourceTLS,
    nativeFullscreen: checkFullscreen ? 'enter-lock-restore-PASS' : 'not-tested',
    malformedAuditRejected: checkInvalidAudit ? 'PASS' : 'not-tested'}));
} finally {
  // Browser.close only affects the browser reached on this isolated CDP port.
  try { await call('Browser.close'); } catch {}
  ws.close();
  for (const entry of pending.values()) clearTimeout(entry.timer);
}
