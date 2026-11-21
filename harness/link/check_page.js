#!/usr/bin/env node
// check_page.js - the web app's own serial code (adsb_scope.htm, as it is)
// against the simulated device (sim_device, which runs the firmware's speed
// state machine), with a stand-in for the browser's serial port that is a
// pseudo-terminal: open and close are real, so "opening the port resets the
// device" is real too.
//
// The page's whole script is run in Node with the browser's objects stubbed;
// nothing is copied out of it. What is exercised: connect, the read loop,
// commands, the link-speed negotiation both ways, keepalive, a device that
// restarts, a device in download mode, disconnect. Not exercised: flashing
// (esptool-js comes from a CDN) and anything drawn.
//
// usage: node check_page.js        (make check runs it; about 3 minutes)
// needs: node 18 or later, ./sim_device built, stty
const fs = require('fs'), vm = require('vm'), path = require('path'), cp = require('child_process');
const HERE = __dirname;
const PAGE = path.join(HERE, '..', '..', 'main', 'examples', 'adsb_receiver', 'adsb_scope.htm');
const SIM = path.join(HERE, '..', 'sim_device');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const fails = [];
function check(cond, what) { console.log((cond ? '  ok    ' : '  FAIL  ') + what); if (!cond) fails.push(what); }

for (const [tool, hint] of [[SIM, 'run make in harness/'], [PAGE, 'the web app']]) if (!fs.existsSync(tool)) { console.error('error: ' + tool + ' not found (' + hint + ')'); process.exit(2); }
if (cp.spawnSync('stty', ['--version']).status !== 0) { console.error('error: stty not found'); process.exit(2); }

const html = fs.readFileSync(PAGE, 'utf8');
const script = [...html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]).sort((a, b) => b.length - a.length)[0];

function startSim(args) {
  return new Promise((res, rej) => {
    const p = cp.spawn(SIM, args, { stdio: ['ignore', 'pipe', 'inherit'] });
    p.stdout.once('data', d => res({ p, path: String(d).split('\n')[0].trim() }));
    p.once('error', rej);
  });
}

// The browser's SerialPort, on a pseudo-terminal
class MockPort {
  constructor(ptyPath, sim) { this.path = ptyPath; this.sim = sim; this.fd = -1; this.readable = null; this.writable = null; this.opens = []; this.locked = false; this.lockClash = 0; }
  async open(opt) {
    if (this.fd >= 0) throw new Error('The port is already open.');
    this.fd = fs.openSync(this.path, fs.constants.O_RDWR | fs.constants.O_NOCTTY | fs.constants.O_NONBLOCK);
    // The speed is set through a second, short-lived open of the same terminal (the first stays open, so the
    // device sees no close). Handing our own descriptor to stty would leave it blocking.
    cp.spawnSync('stty', ['-F', this.path, 'raw', '-echo', String(opt.baudRate)], { stdio: ['ignore', 'ignore', 'inherit'] });
    this.opens.push(opt.baudRate);
    const self = this;
    this.queue = []; this.waiter = null;
    const buf = Buffer.alloc(65536);
    this.timer = setInterval(() => {
      let n = 0;
      try { n = fs.readSync(self.fd, buf, 0, buf.length, null); } catch (e) { n = 0; }
      if (n > 0) {
        const chunk = new Uint8Array(buf.subarray(0, n));
        if (self.waiter) { const w = self.waiter; self.waiter = null; w({ value: chunk, done: false }); } else self.queue.push(chunk);
      }
    }, 3);
    this.readable = {
      getReader() {
        return {
          read() { return new Promise(r => { if (self.queue.length) r({ value: self.queue.shift(), done: false }); else self.waiter = r; }); },
          async cancel() { if (self.waiter) { const w = self.waiter; self.waiter = null; w({ value: undefined, done: true }); } },
          releaseLock() {},
        };
      },
    };
    this.writable = {
      getWriter() {
        if (self.locked) { self.lockClash++; throw new TypeError('Cannot create writer when WritableStream is locked'); }
        self.locked = true;
        return { async write(u8) { await sleep(1); fs.writeSync(self.fd, Buffer.from(u8)); }, releaseLock() { self.locked = false; } };
      },
    };
  }
  async setSignals(sig) {
    // RTS raised with DTR released: the reset line, with the boot pin high
    if (sig.requestToSend === true && sig.dataTerminalReady === false) this.sim.p.kill('SIGUSR2');
  }
  async close() {
    if (this.fd < 0) return;
    clearInterval(this.timer);
    if (this.waiter) { const w = this.waiter; this.waiter = null; w({ value: undefined, done: true }); }
    fs.closeSync(this.fd); this.fd = -1; this.readable = null; this.writable = null; this.locked = false;
  }
  getInfo() { return {}; }
}

// The page, in a context of its own
function loadPage(port, settings) {
  const U = new Proxy(function () {}, {
    get: (t, k) => { if (k === Symbol.toPrimitive) return () => ''; if (k === 'then') return undefined; if (k === Symbol.iterator) return function* () {}; if (k === 'length') return 0; return U; },
    apply: () => U, construct: () => U, set: () => true, has: () => true,
  });
  const store = new Map(Object.entries(settings || {}));
  const ctx = { console, setTimeout, setInterval, clearTimeout, clearInterval, TextDecoder, TextEncoder, URL, Blob: U, Promise, Date, Math, JSON,
    document: U, window: U, L: U, mqtt: U, THREE: U, Image: U, Audio: U, AudioContext: U, WebSocket: U, Worker: U, ResizeObserver: U, IntersectionObserver: U,
    MutationObserver: U, XMLHttpRequest: U, FileReader: U, CustomEvent: U, Event: U, HTMLElement: U, screen: U, history: U, innerWidth: 1200, innerHeight: 800, devicePixelRatio: 1,
    location: { href: 'http://localhost/', search: '', hash: '', protocol: 'http:', host: 'localhost' },
    navigator: { serial: { requestPort: async () => port }, userAgent: 'node' },
    localStorage: { getItem: k => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)), removeItem: k => store.delete(k) },
    fetch: async () => { throw new Error('offline'); }, alert: () => {}, confirm: () => true, requestAnimationFrame: () => 0, performance: { now: () => Date.now() },
    addEventListener: () => {}, matchMedia: () => U, getComputedStyle: () => U, crypto: require('crypto').webcrypto,
    atob: s => Buffer.from(s, 'base64').toString('binary'), btoa: s => Buffer.from(s, 'binary').toString('base64'),
    __logs: [], __lines: [] };
  ctx.globalThis = ctx; ctx.self = ctx;
  vm.createContext(ctx);
  vm.runInContext(script, ctx, { timeout: 10000, filename: 'adsb_scope.htm.js' });
  // Listen in on what the page logs and parses, without changing it
  vm.runInContext('{const a=addLog,p=parse;addLog=function(l){__logs.push(l);return a(l)};parse=function(l){__lines.push(l);return p(l)}}', ctx);
  const q = expr => vm.runInContext(expr, ctx);
  return { ctx, q, logs: ctx.__logs, lines: ctx.__lines };
}

async function until(fn, ms, step = 100) { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (fn()) return true; await sleep(step); } return fn(); }
function seqGaps(lines, n) {
  const s = lines.map(l => /seq=(\d+) \[8D/.exec(l)).filter(Boolean).map(m => +m[1]).slice(-n);
  return s.length > 1 ? s[s.length - 1] - s[0] + 1 - s.length : 1e9;
}

async function scenario(title, simArgs, settings, body) {
  if (process.env.ONLY && !process.env.ONLY.split(',').includes(title.split('.')[0])) return;   // ONLY=1,7 node check_page.js
  console.log(title);
  const sim = await startSim(simArgs);
  const port = new MockPort(sim.path, sim);
  const page = loadPage(port, settings);
  try { await body({ sim, port, page, q: page.q }); }
  catch (e) { check(false, 'scenario ran without an error (' + e.message + ')'); }
  try { await page.q('conn?discon():0'); } catch (e) {}
  try { await port.close(); } catch (e) {}
  sim.p.kill('SIGKILL');
  await sleep(100);
}

(async () => {
  await scenario('1. T-Display-P4: opening the port restarts the device; firmware with `baud`', ['--rate', '250', '--reset-on-open', '--boot-ms', '1500'], {}, async ({ port, page, q }) => {
    await q('serialConnect()');
    check(await until(() => q('linkState') === 'up', 40000), 'the link comes up (state ' + q('linkState') + ')');
    check(JSON.stringify(port.opens) === '[115200,921600]', 'the port was opened at 115200, then once more at 921600 (' + port.opens.join(', ') + ')');
    check(page.logs.some(l => /restarts once more/.test(l)) && page.logs.some(l => /Serial link up at 921600 baud/.test(l)), 'the page says what it is doing');
    check(q('deviceVersion') === 'simdev', 'the version read from the first boot log is kept');
    const n0 = page.lines.length;
    await sleep(5000);
    check(page.lines.length - n0 > 1000, 'lines arrive at 921600 (' + (page.lines.length - n0) + ' in 5 s)');
    check(seqGaps(page.lines, 1000) === 0, 'none missing among the last 1000 (' + seqGaps(page.lines, 1000) + ')');
    check(!page.lines.some(l => /BAUD: ok/.test(l)) && !page.logs.some(l => /BAUD: ok/.test(l)), 'keepalive answers do not reach the log or the parser');
    check(!page.logs.some(l => /�/.test(l)), 'the boot log at the wrong speed is not shown as garbage');
    check(port.lockClash === 0, 'no two writes ever met on the port');
    await sleep(11000);
    check(q('linkState') === 'up', 'still up after 11 s (the keepalive holds the device at 921600)');
    await q('discon()');
    check(q('linkState') === 'base' && q('linkBaud') === 115200, 'disconnect leaves the page at 115200');
  });

  await scenario('2. the same traffic with the setting at 115200', ['--rate', '250', '--reset-on-open', '--boot-ms', '1500'], { link_baud: '115200' }, async ({ port, page, q }) => {
    await q('serialConnect()');
    await until(() => q('consoleReady') === true, 15000);
    await sleep(6000);
    const s = page.lines.map(l => /seq=(\d+) \[8D/.exec(l)).filter(Boolean).map(m => +m[1]).slice(-200);
    const lost = s.length > 1 ? 1 - s.length / (s[s.length - 1] - s[0] + 1) : 0;
    check(JSON.stringify(port.opens) === '[115200]' && q('linkState') === 'base', 'no negotiation, one open');
    check(lost > 0.5, 'most lines are dropped by the device before they are sent (' + Math.round(100 * lost) + ' % of the last stretch)');
  });

  await scenario('3. a device that does not restart when the port is opened', ['--rate', '250'], {}, async ({ port, page, q }) => {
    await q('serialConnect()');
    // no boot log on this one: the console is already up, and says so on its heartbeat; the test stands in for that
    await sleep(500); q('consoleReady=true');
    check(await until(() => q('linkState') === 'up', 20000), 'the link comes up (state ' + q('linkState') + ')');
    check(JSON.stringify(port.opens) === '[115200,921600]' && !page.logs.some(l => /restarts once more/.test(l)), 'asked, reopened, confirmed – no restart');
    await sleep(3000);
    check(seqGaps(page.lines, 500) === 0, 'none missing among the last 500');
  });

  await scenario('4. firmware before coh4', ['--rate', '30', '--reset-on-open', '--boot-ms', '1200', '--old-firmware'], {}, async ({ port, page, q }) => {
    await q('serialConnect()');
    check(await until(() => q('linkState') === 'off', 20000), 'the page gives up on a faster speed (state ' + q('linkState') + ')');
    check(JSON.stringify(port.opens) === '[115200]', 'the port is not reopened');
    check(page.logs.some(l => /cannot change speed/.test(l)), 'and says why');
    const n0 = page.lines.length; await sleep(2000);
    check(page.lines.length > n0, 'lines keep arriving at 115200');
  });

  await scenario('5. no restart on open, and a link that fails above 460800', ['--rate', '150', '--max-baud', '460800'], {}, async ({ port, page, q }) => {
    await q('serialConnect()');
    await sleep(500); q('consoleReady=true');
    check(await until(() => q('linkState') === 'up' || q('linkState') === 'off', 40000) && q('linkState') === 'up' && q('linkBaud') === 460800, 'ends up at 460800 (state ' + q('linkState') + ', ' + q('linkBaud') + ')');
    check(JSON.stringify(port.opens) === '[115200,921600,115200,460800]', '921600 tried and not confirmed, back to 115200, then 460800 (' + port.opens.join(', ') + ')');
  });

  await scenario('6. restart on open, and a link that fails above 460800', ['--rate', '50', '--reset-on-open', '--boot-ms', '800', '--max-baud', '460800'], {}, async ({ port, page, q }) => {
    await q('serialConnect()');
    check(await until(() => q('linkState') === 'off', 75000), 'the page gives up after the restart is not confirmed (state ' + q('linkState') + ')');
    check(JSON.stringify(port.opens) === '[115200,921600,115200]' && q('linkBaud') === 115200, 'and is back at 115200 (' + port.opens.join(', ') + ')');
    await until(() => q('consoleReady') === true, 15000);
    const n0 = page.lines.length; await sleep(3000);
    check(page.lines.length > n0 + 50 && !page.logs.slice(-50).some(l => /�/.test(l)), 'where lines arrive again, readable');
  });

  await scenario('7. the device restarts by itself while the link is up', ['--rate', '100', '--reset-on-open', '--boot-ms', '1200'], {}, async ({ sim, port, page, q }) => {
    await q('serialConnect()');
    check(await until(() => q('linkState') === 'up', 40000), 'up');
    sim.p.kill('SIGUSR1');
    check(await until(() => page.logs.some(l => /no answer at 921600/.test(l)), 15000), 'the page notices within its 8 s');
    check(await until(() => q('linkState') === 'up', 60000), 'and brings the link up again (state ' + q('linkState') + ')');
    check(JSON.stringify(port.opens) === '[115200,921600,115200,921600]', 'by way of 115200 (' + port.opens.join(', ') + ')');
    await sleep(3000);
    check(seqGaps(page.lines, 200) === 0, 'none missing among the last 200');
  });

  await scenario('8. the connect leaves the device in download mode', ['--rate', '100', '--reset-on-open', '--boot-ms', '1200', '--download-first'], {}, async ({ port, page, q }) => {
    await q('serialConnect()');
    check(await until(() => page.logs.some(l => /download mode – restarting it/.test(l)), 5000), 'the page sees "waiting for download" and restarts the device');
    check(await until(() => q('linkState') === 'up', 40000), 'and then brings the link up as usual (state ' + q('linkState') + ')');
  });

  await scenario('9. writes that meet', ['--rate', '10'], { link_baud: '115200' }, async ({ port, page, q }) => {
    await q('serialConnect()');
    await sleep(300);
    await q('Promise.all([serialWrite("\\x05"),serialWrite("\\x05"),serialWrite("\\x05"),serialWrite("\\x05")])');
    check(port.lockClash === 0, 'four writes asked for at once go out one after another');
  });

  console.log();
  if (fails.length) { console.log('check_page: ' + fails.length + ' FAILED'); process.exit(1); }
  console.log('check_page: ok');
  process.exit(0);
})();
