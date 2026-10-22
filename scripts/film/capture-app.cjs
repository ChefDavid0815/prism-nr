// Captures the installed Prism NR window through the DevTools protocol for the promo films.
// Read-only apart from UI state (selection, scroll, a slider): it never presses 一键启用 or 还原.
//   node scripts/film/capture-app.cjs <port> <out dir>
const fs = require('fs');
const path = require('path');
const [port, outDir] = process.argv.slice(2);
fs.mkdirSync(outDir, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  let targets = [];
  for (let i = 0; i < 60 && !targets.some((t) => t.type === 'page'); i++) {
    try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); } catch { /* starting */ }
    await sleep(500);
  }
  const ws = new WebSocket(targets.find((t) => t.type === 'page').webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r));
  let id = 0;
  const pending = new Map();
  ws.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const send = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); setTimeout(() => { if (pending.has(n)) { pending.delete(n); console.log('timeout', method); r({}); } }, 15000); });
  const ev = async (expression) => (await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const shot = async (name) => {
    await sleep(900);
    const r = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(path.join(outDir, name + '.png'), Buffer.from(r.result.data, 'base64'));
    console.log('shot', name);
  };
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 4 / 3, mobile: false });
  for (let i = 0; i < 90; i++) { if (await ev(`[...document.querySelectorAll('.row')].some(r => r.textContent.includes('CONTROL'))`)) break; await sleep(1000); }
  await sleep(4000);
  const pick = (name) => ev(`[...document.querySelectorAll('.row')].find(r => r.textContent.includes(${JSON.stringify(name)}))?.click()`);
  const only = process.env.ONLY ? process.env.ONLY.split(',') : null;
  if (only) {
    for (const step of only) {
      if (step === 'f1') { await pick('F1'); await sleep(1500); await shot('lib-f1-blocked'); }
      if (step === 'plugins') { await ev("[...document.querySelectorAll('button, a, li')].find(b => b.textContent.trim().startsWith('插件'))?.click()"); await shot('plugins'); }
      if (step === 'history') { await ev("[...document.querySelectorAll('button, a, li')].find(b => b.textContent.trim().startsWith('还原记录'))?.click()"); await shot('history'); }
    }
    ws.close();
    return;
  }
  await pick('CONTROL'); await sleep(1800);
  await shot('lib-control');
  await ev(`document.querySelector('.detail-scroll').scrollTo({ top: document.querySelector('.detail-scroll').scrollHeight * 0.36 })`);
  await shot('lib-control-core');
  // Move the intensity slider to show the ticks (the panel writes the live game's ini; set it back after).
  await ev(`(() => { const s = document.querySelectorAll('.tick-range input')[0]; const set = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set; set.call(s, 70); s.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await sleep(1200);
  await shot('lib-control-core-tuned');
  await ev(`(() => { const s = document.querySelectorAll('.tick-range input')[0]; const set = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set; set.call(s, 100); s.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await sleep(1200);
  await ev(`document.querySelector('.detail-scroll').scrollTo({ top: document.querySelector('.detail-scroll').scrollHeight })`);
  await shot('lib-control-bottom');
  await pick('F1'); await sleep(1500);
  await ev(`document.querySelector('.detail-scroll').scrollTo({ top: 0 })`);
  await shot('lib-f1-blocked');
  await ev(`[...document.querySelectorAll('.rail button, nav button, .rail a, .rail li')].find(b => b.textContent.includes('插件'))?.click()`);
  await shot('plugins');
  await ev(`[...document.querySelectorAll('.rail button, nav button, .rail a, .rail li')].find(b => b.textContent.includes('还原记录'))?.click()`);
  await shot('history');
  ws.close();
})().catch((e) => { console.error(e); process.exitCode = 1; });
