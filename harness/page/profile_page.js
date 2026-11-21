#!/usr/bin/env node
// profile_page.js - where the web app's time goes for each line from the
// device, and what it asks of the browser for it.
//
// The page's whole script (adsb_scope.htm, as it is) runs in Node with the
// browser's objects stubbed (link/page_env.js). A made-up device log in the
// device's format - N aircraft, R lines a second, 45 % of them with a
// position - is fed to handleLine() in real time, in 10 ms portions as a
// serial port delivers it, while the page's own timers run. Every top-level
// function of the page is timed (own time and with what it calls), and every
// call the page makes into document, Leaflet and so on is counted by name.
//
// What this measures: the page's own JavaScript, and HOW OFTEN it touches the
// DOM and the map. What it cannot: what those touches cost in a browser
// (style, layout, paint, Leaflet's own work) - the stub answers at once. The
// counts are the pointer to that cost; the page's own meter (add ?perf=1 to
// its address) measures it where it happens.
//
// usage: node profile_page.js [--aircraft N] [--rate R] [--seconds T] [--top K]
//        without arguments: three loads, 12 s each
'use strict';
const fs = require('fs'), path = require('path');
const { loadPage, PAGE } = require('../link/page_env.js');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? Number(process.argv[i + 1]) : d; };

const { fleet, line } = require('./fake_log.js');

async function run(nAc, rate, seconds, top) {
  // timers and frames of the page, timed
  const busy = { feed: 0, timers: 0, frames: 0 };
  const timed = (key, fn) => function () { const t0 = performance.now(); try { return fn.apply(this, arguments); } finally { busy[key] += performance.now() - t0; } };
  const real = { setTimeout: global.setTimeout, setInterval: global.setInterval };
  const handles = [];
  global.setTimeout = (fn, ms, ...a) => real.setTimeout(typeof fn === 'function' ? timed('timers', fn) : fn, ms, ...a);
  global.setInterval = (fn, ms, ...a) => { const h = real.setInterval(typeof fn === 'function' ? timed('timers', fn) : fn, ms, ...a); handles.push(h); return h; };
  const counts = { get: {}, call: {}, set: {} };
  let page;
  try { page = loadPage(null, {}, { counts, raf: fn => real.setTimeout(timed('frames', () => fn(performance.now())), 16) }); }
  finally { global.setTimeout = real.setTimeout; global.setInterval = real.setInterval; }

  // every top-level function of the page, timed
  const src = fs.readFileSync(PAGE, 'utf8');
  const names = [...new Set([...src.matchAll(/^(?:async )?function ([A-Za-z_$][\w$]*)\s*\(/gm)].map(m => m[1]))].filter(n => { try { return page.q('typeof ' + n) === 'function'; } catch (e) { return false; } });
  const stats = names.map(n => ({ name: n, calls: 0, incl: 0, self: 0 }));
  const stack = [];
  page.ctx.__enter = i => { stack.push([i, performance.now(), 0]); };
  page.ctx.__exit = () => { const e = stack.pop(), dt = performance.now() - e[1], s = stats[e[0]]; s.calls++; s.incl += dt; s.self += dt - e[2]; if (stack.length) stack[stack.length - 1][2] += dt; };
  names.forEach((n, i) => page.q('{const f=' + n + ';' + n + '=function(){__enter(' + i + ');try{return f.apply(this,arguments)}finally{__exit()}}}'));
  page.q('conn=true');

  const f = fleet(nAc);
  const WARM = 3;
  let t = 0, fed = 0, carry = 0, t0 = 0;
  const reset = () => { for (const s of stats) { s.calls = 0; s.incl = 0; s.self = 0; } busy.feed = busy.timers = busy.frames = 0; fed = 0; for (const m of [counts.get, counts.call, counts.set]) for (const k of Object.keys(m)) delete m[k]; t0 = performance.now(); };
  const start = performance.now(); let warmed = false;
  while ((performance.now() - start) / 1000 < seconds + WARM) {
    const now = (performance.now() - start) / 1000;
    if (!warmed && now >= WARM) { warmed = true; reset(); }
    carry += rate * (now - t); t = now;
    const n = Math.floor(carry); carry -= n;
    const a = performance.now();
    for (let i = 0; i < n; i++) page.ctx.handleLine(line(f, 20 + t));
    if (n && Math.floor(now) !== Math.floor(now - 0.01)) page.ctx.handleLine('I (' + Math.floor(now * 1000) + ') CLASS: SD flush: 869 bytes in 9 ms');
    busy.feed += performance.now() - a; fed += n;
    await sleep(10);
  }
  const secs = (performance.now() - t0) / 1000;
  for (const h of handles) clearInterval(h);

  const total = busy.feed + busy.timers + busy.frames;
  console.log('\n== ' + nAc + ' aircraft, ' + rate + ' lines/s, ' + secs.toFixed(1) + ' s measured after ' + WARM + ' s warm-up ==');
  console.log('page JavaScript: ' + (total / secs).toFixed(1) + ' ms per second (' + (total / secs / 10).toFixed(1) + ' % of one core here): lines ' + (busy.feed / secs).toFixed(1) + ', timers ' + (busy.timers / secs).toFixed(1) + ', frames ' + (busy.frames / secs).toFixed(1));
  console.log('per line: ' + (busy.feed / fed * 1000).toFixed(1) + ' us   (' + fed + ' lines)');
  console.log('\n  own ms/s   with callees   calls/s   function');
  for (const s of stats.filter(s => s.calls).sort((a, b) => b.self - a.self).slice(0, top))
    console.log('  ' + (s.self / secs).toFixed(2).padStart(8) + '   ' + (s.incl / secs).toFixed(2).padStart(12) + '   ' + (s.calls / secs).toFixed(0).padStart(7) + '   ' + s.name);
  const rows = [];
  for (const [kind, m] of [['call', counts.call], ['set', counts.set]]) for (const [k, v] of Object.entries(m)) rows.push([v / secs, kind === 'set' ? k + ' =' : k + '()']);
  console.log('\n  per second   what the page asks of the browser (document, elements, Leaflet)');
  for (const [v, k] of rows.sort((a, b) => b[0] - a[0]).slice(0, top + 6)) console.log('  ' + v.toFixed(0).padStart(10) + '   ' + k);
}

(async () => {
  const top = arg('top', 14);
  if (process.argv.includes('--rate') || process.argv.includes('--aircraft')) await run(arg('aircraft', 60), arg('rate', 300), arg('seconds', 12), top);
  else for (const [n, r] of [[20, 50], [60, 300], [120, 600]]) await run(n, r, arg('seconds', 12), top);
  process.exit(0);
})().catch(e => { console.error(e); process.exit(2); });
