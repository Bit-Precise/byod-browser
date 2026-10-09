// Isolated fixture only. Usage: node smoke-native-audit.mjs fixture.json CDP_PORT OUTER_PORT
import {readFileSync} from 'node:fs';
import {get} from 'node:https';
import assert from 'node:assert/strict';
const fixture = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const cdp = `http://127.0.0.1:${Number(process.argv[3])}`;
const outerPort = Number(process.argv[4]);
const delay = ms => new Promise(r => setTimeout(r, ms));
async function stats() {
  return new Promise((resolve, reject) => {
    get(fixture.front + '/fixture/stats', {ca: fixture.certificate,
      headers: {Authorization: 'Bearer ' + fixture.ticket}}, r => {
      let body = ''; r.on('data', b => body += b); r.on('end', () => {
        try { assert.equal(r.statusCode, 200); resolve(JSON.parse(body)); } catch(e) { reject(e); }
      }); r.on('error', reject);
    }).on('error', reject);
  });
}
const previousIDs = new Set((await stats()).events.map(e => e.id));
let target;
for(let i=0;i<60 && !target;i++) {
  try { target = (await (await fetch(cdp+'/json/list')).json()).find(t => t.type === 'page' && t.url.startsWith('grips://exam.cs.ac.cn')); } catch {}
  if (!target) await delay(500);
}
assert(target, 'isolated grips tab not found');
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve,reject) => {ws.addEventListener('open',resolve,{once:true});ws.addEventListener('error',reject,{once:true});});
const pending = new Map(); let id = 0;
ws.addEventListener('message', e => {
  const m=JSON.parse(e.data); const p=pending.get(m.id); if(!p)return;
  pending.delete(m.id); clearTimeout(p.timer); m.error ? p.reject(Error(JSON.stringify(m.error))) : p.resolve(m.result);
});
function call(method,params={}) {return new Promise((resolve,reject)=>{
  const key=++id; const timer=setTimeout(()=>{pending.delete(key);reject(Error('timeout '+method));},45000);
  pending.set(key,{resolve,reject,timer});ws.send(JSON.stringify({id:key,method,params}));
});}
async function evaluate(expression) {
  const r=await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
  assert(!r.exceptionDetails, JSON.stringify(r.exceptionDetails)); return r.result.value;
}
async function eventMatching(predicate) {
  for(let i=0;i<100;i++) { const events=(await stats()).events.filter(e=>e.audit && !previousIDs.has(e.id)); const e=events.find(predicate); if(e)return e; await delay(200); }
  throw Error('missing expected audit event: '+JSON.stringify((await stats()).events));
}
async function navigate(url) {await call('Page.navigate',{url});await delay(600);}
const config={sourceHost:'127.0.0.1',tunnelHosts:['127.0.0.1'],proxyHost:'exam.cs.ac.cn',proxyPort:outerPort,
 endpointId:'course-101',ticket:fixture.ticket,allowedOrigins:[fixture.source],examEndsAtMs:0,
 auditSessionId:fixture.audit_session_id,auditToken:fixture.audit_token};
const source=fixture.source;
const direct=`https://exam.cs.ac.cn:${outerPort}`;
try {
  await call('Page.enable');await call('Runtime.enable');
  await evaluate(`chrome.send('setByodTunnelConfig',[${JSON.stringify(config)}]); true`);
  await eventMatching(e=>e.type==='exam_entered');
  await navigate(source+'/native-smoke?token=secret#fragment');
  await eventMatching(e=>e.type==='page_navigation' && e.audit.url===source+'/native-smoke' && e.audit.route==='tunnel');
  await evaluate(`Promise.all([fetch('/fetch-only?secret=1'), new Promise(resolve=>{const x=new XMLHttpRequest(); x.open('GET','/xhr-only');x.onload=resolve;x.send();}), new Promise(resolve=>{const i=new Image();i.onload=i.onerror=resolve;i.src='/image-only';})]).then(()=>true)`);
  await delay(500);
  assert(!(await stats()).events.some(e=>e.audit?.url?.includes('-only')), 'subresources leaked into page audit');
  await navigate(direct+'/direct?token=secret');
  await eventMatching(e=>e.type==='page_navigation' && e.audit.url===direct+'/direct' && e.audit.route==='direct');
  await navigate(source+'/redirect?token=secret');
  await eventMatching(e=>e.type==='page_redirect' && e.audit.url===source+'/redirect' && e.audit.redirect_url===source+'/native-smoke' && e.audit.route==='tunnel');
  await evaluate("history.pushState({}, '', '/same-document?token=secret#fragment'); true");
  await eventMatching(e=>e.audit.url===source+'/same-document' && e.audit.route==='none' && e.audit.route_reason==='same_document');
  await navigate(source+'/cache-page');
  await navigate(direct+'/direct');
  await navigate(source+'/cache-page');
  await eventMatching(e=>e.audit.url===source+'/cache-page' && e.audit.route==='none' && ['cache','back_forward_cache'].includes(e.audit.route_reason));
  await navigate('https://byod-audit-failure.invalid/');
  await eventMatching(e=>e.audit.url==='https://byod-audit-failure.invalid/' && e.audit.outcome==='failed' && e.audit.route==='unknown');
  await navigate('grips://exam.cs.ac.cn/');
  await eventMatching(e=>e.type==='exam_left' && e.audit.reason==='control_page');
  await evaluate("chrome.send('clearByodTunnelConfig', []); true");
  const count=(await stats()).events.length;
  await navigate(direct+'/outside-exam');await delay(400);
  assert.equal((await stats()).events.length,count,'normal browsing audited after leave');
  // Native deadline must emit a distinct leave reason even after the shell
  // has been replaced by the source page.
  await navigate('grips://exam.cs.ac.cn/');
  await evaluate(`chrome.send('setByodTunnelConfig',[{...${JSON.stringify(config)},examEndsAtMs:Date.now()+1800}]); true`);
  await navigate(source+'/expiry-page');
  await eventMatching(e=>e.audit.url===source+'/expiry-page' && e.audit.outcome==='committed');
  await eventMatching(e=>e.type==='exam_left' && e.audit.reason==='expired');
  await navigate('grips://exam.cs.ac.cn/');
  // A real renderer crash must remain distinct from fast tab shutdown.
  await evaluate(`chrome.send('setByodTunnelConfig',[${JSON.stringify(config)}]); true`);
  await navigate(source+'/crash-page');
  await eventMatching(e=>e.audit.url===source+'/crash-page');
  void call('Page.crash').catch(()=>{});
  await eventMatching(e=>e.type==='exam_left' && e.audit.reason==='renderer_gone');
  await navigate('grips://exam.cs.ac.cn/');
  // Leave another isolated blank tab alive so tab close does not terminate
  // the browser process before its best-effort final POST can finish.
  await call('Target.createTarget',{url:'about:blank',background:true});
  await evaluate(`chrome.send('setByodTunnelConfig',[${JSON.stringify(config)}]); true`);
  await navigate(source+'/close-page');
  await eventMatching(e=>e.audit.url===source+'/close-page');
  await fetch(cdp+'/json/close/'+target.id);
  await eventMatching(e=>e.type==='exam_left' && e.audit.reason==='tab_closed');
  const events=(await stats()).events.filter(e=>e.audit && !previousIDs.has(e.id));
  for(const e of events) {
    assert(!JSON.stringify(e.audit).includes('secret') && !JSON.stringify(e.audit).includes('#fragment'), 'URL secrets leaked');
    assert(Number.isInteger(e.audit.observed_at_ms));
  }
  console.log(JSON.stringify({nativeAudit:'PASS', events:events.length, tunnel:true,direct:true,redirect:true,sameDocument:true,cache:true,failure:true,leave:true,expiry:true,rendererCrash:true,tabClose:true,subresourcesExcluded:true,normalBrowsingExcluded:true,urlRedaction:true}));
} finally {
  try {
    const info=await (await fetch(cdp+'/json/version')).json();
    const browser=new WebSocket(info.webSocketDebuggerUrl);
    await new Promise(resolve=>{
      const timer=setTimeout(()=>{browser.close();resolve();},3000);
      browser.addEventListener('open',()=>browser.send(JSON.stringify({id:1,method:'Browser.close'})),{once:true});
      browser.addEventListener('close',()=>{clearTimeout(timer);resolve();},{once:true});
      browser.addEventListener('error',()=>{clearTimeout(timer);resolve();},{once:true});
    });
  } catch {}
  ws.close();for(const p of pending.values())clearTimeout(p.timer);
}
