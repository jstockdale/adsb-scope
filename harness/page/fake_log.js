// fake_log.js - a made-up device log in the device's console format, for
// profile_page.js (Node) and browser_profile.js (injected into the page).
// N aircraft around a point; each line is one decoded message from one of
// them, 45 % with a position (which then moves on by half a second of flight).
(function (g) {
  'use strict';
  function fleet(n) {
    const ac = [];
    let x = 12345;
    const rnd = () => { x = (x * 1664525 + 1013904223) >>> 0; return x / 4294967296; };
    for (let i = 0; i < n; i++) {
      ac.push({ ic: (0xA00000 + Math.floor(rnd() * 0xFFFFF)).toString(16).toUpperCase(), cs: rnd() < 0.8 ? ['UAL', 'SWA', 'DAL', 'ASA', 'SKW', 'N'][Math.floor(rnd() * 6)] + (100 + Math.floor(rnd() * 8900)) : '',
                lat: 37.6 + (rnd() - 0.5) * 2.4, lon: -122.4 + (rnd() - 0.5) * 3.0, alt: 1000 + Math.floor(rnd() * 380) * 100, kt: 150 + Math.floor(rnd() * 350),
                hdg: Math.floor(rnd() * 360), fpm: (Math.floor(rnd() * 60) - 30) * 64 });
    }
    return { ac, rnd };
  }
  function line(f, t) {
    const a = f.ac[Math.floor(f.rnd() * f.ac.length)], withPos = f.rnd() < 0.45, withCs = a.cs && f.rnd() < 0.5;
    let s = '[boot+' + t.toFixed(3) + '] ' + a.ic + ' ' + (withCs ? a.cs.padEnd(8) + ' ' : ' ') + a.alt + 'ft ' + a.kt + 'kt ' + String(a.hdg).padStart(3, '0') + '° ' + (a.fpm >= 0 ? '+' : '') + a.fpm + 'fpm';
    if (withPos) {
      const d = a.kt * 0.5 / 3600 / 60;
      a.lat += d * Math.cos(a.hdg * Math.PI / 180); a.lon += d * Math.sin(a.hdg * Math.PI / 180) / Math.cos(a.lat * Math.PI / 180);
      s += ' (' + Math.abs(a.lat).toFixed(4) + (a.lat >= 0 ? 'N' : 'S') + ',' + Math.abs(a.lon).toFixed(4) + (a.lon >= 0 ? 'E' : 'W') + ')';
    }
    return s + ' [8D' + a.ic + '58AF8108320BDD94D1C8]';
  }
  const api = { fleet, line };
  if (typeof module !== 'undefined' && module.exports) module.exports = api; else g.fakeLog = api;
})(typeof window !== 'undefined' ? window : globalThis);
