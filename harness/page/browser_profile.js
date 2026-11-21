#!/usr/bin/env node
// browser_profile.js - the web app in a real Chrome (headless), under a
// made-up device log, measured by the browser itself.
//
// The page is served from this machine as it is; its libraries (Leaflet,
// three.js, mqtt) come from local copies instead of the CDNs, map tiles are a
// blank image, everything else outside is refused. A device log (fake_log.js)
// is fed to the page's handleLine() in 10 ms portions, as a serial port
// delivers it, with the log panel closed and open. Chrome's own counters
// (Performance.getMetrics) give script, style and layout time and the number
// of layouts; the page side gives frames and long tasks.
//
// It also is the one place where the page is loaded by a real browser: any
// script error on load is reported and fails the run.
//
// needs: Chrome or Chromium (CHROME=<path>; otherwise google-chrome,
//        chromium, chromium-browser on PATH) and "make page-deps" (npm:
//        puppeteer-core, leaflet, three, mqtt into harness/.cache/web)
// usage: node browser_profile.js [--seconds T] [--loads 60x0,60x300,150x1000]
//        PAGE=<another adsb_scope.htm> node browser_profile.js      to compare versions
'use strict';
const fs = require('fs'), path = require('path'), http = require('http'), { execSync } = require('child_process');
const WEB = path.join(__dirname, '..', '.cache', 'web', 'node_modules');
const PAGE = process.env.PAGE || path.join(__dirname, '..', '..', 'main', 'examples', 'adsb_receiver', 'adsb_scope.htm');   // PAGE=<file>: another version of the page
const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? process.argv[i + 1] : d; };
function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  for (const c of ['google-chrome', 'chromium', 'chromium-browser', 'google-chrome-stable']) { try { return execSync('command -v ' + c, { encoding: 'utf8' }).trim(); } catch (e) { /* next */ } }
  return null;
}
const PNG1 = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==', 'base64');

(async () => {
  const chrome = findChrome();
  if (!fs.existsSync(path.join(WEB, 'puppeteer-core')) || !chrome) {
    console.log('SKIPPED: page/browser_profile.js - ' + (!chrome ? 'no Chrome or Chromium found (CHROME=<path>)' : 'its npm packages are not here ("make page-deps")'));
    return;
  }
  const puppeteer = require(path.join(WEB, 'puppeteer-core'));
  const local = [
    [/leaflet@[\d.]+\/dist\/leaflet\.js/, 'leaflet/dist/leaflet.js', 'application/javascript'],
    [/leaflet@[\d.]+\/dist\/leaflet\.css/, 'leaflet/dist/leaflet.css', 'text/css'],
    [/three\.js\/r\d+\/three\.min\.js/, 'three/build/three.min.js', 'application/javascript'],
    [/mqtt@[\d.]+\/dist\/mqtt\.min\.js/, 'mqtt/dist/mqtt.min.js', 'application/javascript'],
  ];
  const server = http.createServer((q, r) => { r.writeHead(200, { 'content-type': 'text/html; charset=utf-8' }); r.end(fs.readFileSync(PAGE)); });
  await new Promise(res => server.listen(0, '127.0.0.1', res));
  const url = 'http://127.0.0.1:' + server.address().port + '/';
  const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: ['--no-sandbox', '--window-size=1400,900'], defaultViewport: { width: 1400, height: 900 } });
  const page = await browser.newPage();
  const errors = [], refused = new Set();
  page.on('pageerror', e => errors.push(String(e.message || e)));
  await page.setRequestInterception(true);
  page.on('request', req => {
    const u = req.url();
    if (u.startsWith(url)) return req.continue();
    for (const [re, file, type] of local) if (re.test(u)) return req.respond({ status: 200, contentType: type, body: fs.readFileSync(path.join(WEB, file)) });
    if (/basemaps\.cartocdn\.com|tile\.openstreetmap|\.png(\?|$)/.test(u)) return req.respond({ status: 200, contentType: 'image/png', body: PNG1 });
    refused.add(u.replace(/^(https?:\/\/[^/]+).*/, '$1'));
    req.abort();
  });
  await page.goto(url, { waitUntil: 'load', timeout: 60000 });
  await new Promise(r => setTimeout(r, 1500));
  const ver = await page.evaluate(() => (document.querySelector('.scope-ver') || {}).textContent || '?');
  console.log('page ' + ver + ' in ' + (await browser.version()) + '; refused: ' + [...refused].join(', '));
  console.log(errors.length ? 'SCRIPT ERRORS ON LOAD:\n  ' + errors.join('\n  ') : 'loaded without a script error');
  await page.addScriptTag({ content: fs.readFileSync(path.join(__dirname, 'fake_log.js'), 'utf8') });
  const cdp = await page.createCDPSession();
  await cdp.send('Performance.enable');
  const metrics = async () => Object.fromEntries((await cdp.send('Performance.getMetrics')).metrics.map(m => [m.name, m.value]));

  const seconds = Number(arg('seconds', 10));
  const loads = arg('loads', '60x0,60x300,150x1000').split(',').map(s => s.split('x').map(Number));     // aircraft x lines a second; x0: the page left alone
  const feed = (nAc, rate, secs) => new Promise(done => {       // runs in the page
    const f = window.fakeLog.fleet(nAc), t0 = performance.now();
    let fed = 0, frames = 0, worst = 0, lastF = t0, longN = 0, longMs = 0;
    let po = null; try { po = new PerformanceObserver(l => { for (const e of l.getEntries()) { longN++; longMs += e.duration; } }); po.observe({ entryTypes: ['longtask'] }); } catch (e) { /* not there */ }
    const raf = ts => { frames++; worst = Math.max(worst, ts - lastF); lastF = ts; if (performance.now() - t0 < secs * 1000) requestAnimationFrame(raf); };
    requestAnimationFrame(raf);
    const iv = setInterval(() => {
      const t = (performance.now() - t0) / 1000, due = Math.floor(t * rate);
      while (fed < due) { handleLine(window.fakeLog.line(f, 20 + t)); fed++; }                                // eslint-disable-line no-undef
      if (t >= secs) { clearInterval(iv); if (po) po.disconnect(); done({ fed, frames, worst, longN, longMs, secs: t }); }
    }, 10);
  });
  const feedSrc = feed.toString();
  console.log('\n aircraft  lines/s  log panel |  script   style   layout   all main-thread work (ms per second) | layouts/s | frames/s  worst frame  long tasks');
  for (const [nAc, rate] of loads) for (const logOpen of [false, true]) {
    await page.evaluate(open => { const p = document.getElementById('log-panel'); if (p.classList.contains('visible') !== open) toggleLog(); conn = true; }, logOpen);   // eslint-disable-line no-undef
    await page.evaluate(feed, nAc, rate || 300, 2);                // warm-up, also fills the map and the table
    const m0 = await metrics();
    const r = await page.evaluate(feed, nAc, rate, seconds);
    const m1 = await metrics();
    const d = k => (m1[k] - m0[k]);
    const ms = k => (d(k) * 1000 / r.secs).toFixed(0).padStart(6);
    console.log(String(nAc).padStart(9) + String(rate).padStart(9) + (logOpen ? '     open ' : '   closed ') + ' | ' + ms('ScriptDuration') + '  ' + ms('RecalcStyleDuration') + '  ' + ms('LayoutDuration') + '  ' + ms('TaskDuration') + '                                | ' +
      (d('LayoutCount') / r.secs).toFixed(0).padStart(9) + ' | ' + (r.frames / r.secs).toFixed(0).padStart(8) + (r.worst.toFixed(0) + ' ms').padStart(13) + (r.longN + ' (' + r.longMs.toFixed(0) + ' ms)').padStart(15));
  }
  // the page's own meter (?perf=1), next to what Chrome counted for the same load
  const meter = await page.evaluate(async feedSrc => {
    if (typeof scopePerf !== 'function') return null;                                                          // eslint-disable-line no-undef
    scopePerf(true);                                                                                           // eslint-disable-line no-undef
    const feed = eval('(' + feedSrc + ')');
    await feed(60, 300, 11);
    const lines = [...document.getElementById('log-output').children].map(d => d.textContent).filter(t => /^\[scope\] perf: \d/.test(t));
    scopePerf(false);                                                                                          // eslint-disable-line no-undef
    return lines;
  }, feedSrc);
  if (meter === null) console.log('\n(this version of the page has no meter)');
  else {
    console.log("\nthe page's own meter at 60 aircraft, 300 lines/s, log open:\n  " + (meter.length ? meter.join('\n  ') : 'NO LINE FROM THE METER'));
    if (!meter.length) errors.push('the page\'s meter wrote no line');
  }
  const late = errors.length ? '\nSCRIPT ERRORS:\n  ' + [...new Set(errors)].join('\n  ') : '';
  console.log(late);
  await browser.close(); server.close();
  process.exit(errors.length ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
