// page_env.js - the web app (adsb_scope.htm) loaded into Node: its whole
// script, as it is, in a context where the browser's objects are stand-ins.
// Used by check_page.js, check_page_flash.js and page/profile_page.js.
const fs = require('fs'), vm = require('vm'), path = require('path');
const PAGE = path.join(__dirname, '..', '..', 'main', 'examples', 'adsb_receiver', 'adsb_scope.htm');

function pageScript() {
  const html = fs.readFileSync(PAGE, 'utf8');
  return [...html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]).sort((a, b) => b.length - a.length)[0];
}

// port: what navigator.serial.requestPort() gives the page.
// settings: the page's localStorage to start with.
// hooks.confirm(text): the answer to a confirm() dialog (default: true).
// hooks.counts: an object {get,call,set} to count, by name, what the page asks
//   of the browser's objects (document, Leaflet, ...): the last name read
//   before a call is the name the call is counted under.
// hooks.raf: a requestAnimationFrame for the page (default: never calls back).
function loadPage(port, settings, hooks) {
  hooks = hooks || {};
  // An object that answers anything: every property is itself, every call returns itself
  const C = hooks.counts || null; let last = '';
  const bump = (m, k) => { if (typeof k === 'string') m[k] = (m[k] || 0) + 1; };
  const U = new Proxy(function () {}, {
    get: (t, k) => { if (k === Symbol.toPrimitive) return () => ''; if (k === 'then') return undefined; if (k === Symbol.iterator) return function* () {}; if (k === 'length') return 0;
                     if (C) {       // counting: also answer the few questions whose answer decides what the page then does
                       const prev = last; last = k; bump(C.get, k);
                       if (k === 'contains' && prev === 'classList') return () => { bump(C.call, 'classList.contains'); return false; };   // no panel collapsed, nothing hidden
                       if (k === 'checked') return true;                                                                                   // every log filter on, log live
                       if (k === 'textContent' || k === 'value' || k === 'innerHTML') return '';
                     }
                     return U; },
    apply: () => { if (C) bump(C.call, last); return U; }, construct: () => { if (C) bump(C.call, 'new ' + String(last)); return U; },
    set: (t, k) => { if (C) bump(C.set, k); return true; }, has: () => true,
  });
  const store = new Map(Object.entries(settings || {}));
  const ctx = { console, setTimeout, setInterval, clearTimeout, clearInterval, TextDecoder, TextEncoder, URL, Blob: U, Promise, Date, Math, JSON,
    document: U, window: U, L: U, mqtt: U, THREE: U, Image: U, Audio: U, AudioContext: U, WebSocket: U, Worker: U, ResizeObserver: U, IntersectionObserver: U,
    MutationObserver: U, XMLHttpRequest: U, FileReader: U, CustomEvent: U, Event: U, HTMLElement: U, screen: U, history: U, innerWidth: 1200, innerHeight: 800, devicePixelRatio: 1,
    location: { href: 'http://localhost/', search: '', hash: '', protocol: 'http:', host: 'localhost' },
    navigator: { serial: { requestPort: async () => port }, userAgent: 'node' },
    localStorage: { getItem: k => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)), removeItem: k => store.delete(k) },
    fetch: async () => { throw new Error('offline'); }, alert: () => {}, requestAnimationFrame: hooks.raf || (() => 0), performance: { now: () => performance.now() },
    confirm: text => { ctx.__confirms.push(text); return hooks.confirm ? hooks.confirm(text) : true; },
    addEventListener: () => {}, matchMedia: () => U, getComputedStyle: () => U, crypto: require('crypto').webcrypto,
    atob: s => Buffer.from(s, 'base64').toString('binary'), btoa: s => Buffer.from(s, 'binary').toString('base64'),
    __logs: [], __lines: [], __flashLogs: [], __confirms: [] };
  ctx.globalThis = ctx; ctx.self = ctx;
  vm.createContext(ctx);
  vm.runInContext(pageScript(), ctx, { timeout: 10000, filename: 'adsb_scope.htm.js' });
  // Listen in on what the page logs and parses, without changing it
  vm.runInContext('{const a=addLog,p=parse,f=flashLog;addLog=function(l){__logs.push(l);return a(l)};parse=function(l){__lines.push(l);return p(l)};' +
                  'flashLog=function(m,c){__flashLogs.push(String(m));return f(m,c)}}', ctx);
  const q = expr => vm.runInContext(expr, ctx);
  return { ctx, q, logs: ctx.__logs, lines: ctx.__lines, flashLogs: ctx.__flashLogs, confirms: ctx.__confirms, store };
}

module.exports = { loadPage, PAGE };
