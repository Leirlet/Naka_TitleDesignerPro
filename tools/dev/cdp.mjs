// node cdp.mjs [--eval "<js>"] [--shot out.png]
import fs from 'node:fs';
const args = process.argv.slice(2);
const opt = {};
for (let i = 0; i < args.length; i += 2) opt[args[i].replace(/^--/, '')] = args[i + 1];
const pages = await (await fetch('http://127.0.0.1:9333/json')).json();
const ws = new WebSocket(pages.find((p) => p.type === 'page').webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let id = 0;
const waiting = new Map();
ws.onmessage = (e) => { const m = JSON.parse(e.data); if (waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); } };
const call = (method, params) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
setTimeout(() => { console.log('timeout'); process.exit(2); }, 20000);
if (opt.eval) {
  const r = await call('Runtime.evaluate', { expression: opt.eval, returnByValue: true, awaitPromise: true });
  if (r.result.exceptionDetails) console.log('EXC', JSON.stringify(r.result.exceptionDetails.exception?.description || r.result.exceptionDetails));
  else console.log(typeof r.result.result.value === 'string' ? r.result.result.value : JSON.stringify(r.result.result.value, null, 1));
}
if (opt.wait) await new Promise((r) => setTimeout(r, Number(opt.wait)));
if (opt.shot) {
  const r = await call('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(opt.shot, Buffer.from(r.result.data, 'base64'));
  console.log('saved', opt.shot);
}
if (opt.console) {
  // nothing
}
process.exit(0);
