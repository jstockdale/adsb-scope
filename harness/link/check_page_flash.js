#!/usr/bin/env node
// check_page_flash.js - the web app's flashing code (doFlash in
// adsb_scope.htm, as it is) against a stand-in for esptool-js that writes to
// a flash memory held in the test: does "keep settings" leave the settings
// partition alone, and only when it should?
//
// The stand-in has the calls the page uses, with the signatures of
// esptool-js 0.5.4 (ESPLoader: main, readFlash, writeFlash, hardReset;
// Transport: disconnect), and erases whole 4 kB sectors under what it
// writes, as the loader does. The image is made up here in the layout of the
// real one (boot loader 0x2000, partition table 0x8000, application 0x10000);
// IMAGE=<merged .bin> runs the same cases on a real image.
//
// usage: node check_page_flash.js      (about 30 s; part of make check)
const fs = require('fs');
const { loadPage } = require('./page_env.js');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const fails = [];
function check(cond, what) { console.log((cond ? '  ok    ' : '  FAIL  ') + what); if (!cond) fails.push(what); }

// ---- partition tables, images, devices ----
function entry(type, sub, offset, size, label) {
  const e = Buffer.alloc(32, 0); e[0] = 0xAA; e[1] = 0x50; e[2] = type; e[3] = sub; e.writeUInt32LE(offset, 4); e.writeUInt32LE(size, 8); e.write(label, 12, 'latin1'); return e;
}
function table(entries) {
  const md5 = Buffer.alloc(32, 0xFF); md5[0] = 0xEB; md5[1] = 0xEB;
  const t = Buffer.alloc(0xC00, 0xFF); Buffer.concat([...entries, md5]).copy(t); return t;
}
const T_NOW = table([entry(1, 2, 0x9000, 0x6000, 'nvs'), entry(1, 1, 0xF000, 0x1000, 'phy_init'), entry(0, 0, 0x10000, 0xF00000, 'factory')]);
const T_APRIL = table([entry(1, 2, 0x9000, 0x6000, 'nvs'), entry(1, 0, 0xF000, 0x2000, 'otadata'), entry(1, 1, 0x11000, 0x1000, 'phy_init'),
                       entry(0, 0x10, 0x20000, 0x780000, 'ota_0'), entry(0, 0x11, 0x7A0000, 0x780000, 'ota_1')]);
const T_ELSEWHERE = table([entry(1, 2, 0x12000, 0x6000, 'nvs'), entry(0, 0, 0x20000, 0x300000, 'factory')]);
function noise(n, seed) { const b = Buffer.alloc(n); let x = seed >>> 0; for (let i = 0; i < n; i++) { x = (x * 1664525 + 1013904223) >>> 0; b[i] = x >>> 24; } return b; }
function mergedImage() {
  if (process.env.IMAGE) return new Uint8Array(fs.readFileSync(process.env.IMAGE));
  const img = Buffer.alloc(0x10000 + 300000, 0xFF);
  const bl = noise(0x5000, 1); bl[0] = 0xE9; bl.copy(img, 0x2000);
  T_NOW.copy(img, 0x8000);
  const app = noise(300000, 2); app[0] = 0xE9; app.copy(img, 0x10000);
  return new Uint8Array(img);
}
const FLASH = 0x800000;
function device(tbl) {       // a device that has been in use: an older boot loader and application, and settings
  const f = Buffer.alloc(FLASH, 0xFF);
  if (!tbl) return f;        // never flashed
  const bl = noise(0x4000, 11); bl[0] = 0xE9; bl.copy(f, 0x2000);
  tbl.copy(f, 0x8000);
  noise(0x6000, 12).copy(f, 0x9000);          // what is at 0x9000: the settings, on the layouts that have them there
  noise(0x6000, 13).copy(f, 0x12000);
  noise(400000, 14).copy(f, 0x20000);
  return f;
}

// ---- esptool-js, as far as the page uses it ----
function esptoolStandIn(flash, opt) {
  const seen = { speeds: [], reads: [], writes: [], resets: 0 };
  class Transport { constructor(port) { this.port = port; } async disconnect() {} }
  class ESPLoader {
    constructor(o) { this.o = o; seen.speeds.push(o.romBaudrate); if (o.baudrate !== o.romBaudrate) throw new Error('the page asked for a speed change (a reopen)'); }
    async main() { if (opt.noAnswerAbove && this.o.romBaudrate > opt.noAnswerAbove) throw new Error('Failed to connect with the device: No serial data received.'); return 'ESP32-P4 (stand-in)'; }
    async readFlash(addr, size) { seen.reads.push([addr, size]); if (opt.readFails) throw new Error('Failed to read memory: 5'); return new Uint8Array(flash.subarray(addr, addr + size)); }
    async writeFlash(o) {
      for (const [i, f] of o.fileArray.entries()) {
        if (typeof f.data !== 'string') throw new Error('writeFlash wants the data as a binary string');
        const data = Buffer.from(f.data, 'latin1');
        if (opt.writeFailsAbove && this.o.romBaudrate > opt.writeFailsAbove) { flash.fill(0xFF, f.address, f.address + 4096); throw new Error('Timeout'); }
        flash.fill(0xFF, Math.floor(f.address / 4096) * 4096, Math.ceil((f.address + data.length) / 4096) * 4096);   // sectors erased
        data.copy(flash, f.address);
        seen.writes.push([f.address, data.length, this.o.romBaudrate]);
        if (o.reportProgress) { o.reportProgress(i, Math.floor(data.length / 2), data.length); o.reportProgress(i, data.length, data.length); }
      }
    }
    async hardReset() { seen.resets++; }
  }
  return { mod: { Transport, ESPLoader }, seen };
}
const fakePort = () => ({ async open() {}, async close() {}, async setSignals() {}, getInfo() { return {}; }, readable: null, writable: null });

function same(a, b, lo, hi) { return Buffer.compare(Buffer.from(a.buffer, a.byteOffset + lo, hi - lo), Buffer.from(b.buffer, b.byteOffset + lo, hi - lo)) === 0; }

async function flashCase(title, o, body) {
  if (process.env.ONLY && !process.env.ONLY.split(',').includes(title.split('.')[0])) return;
  console.log(title);
  const img = o.image || mergedImage();
  const flash = device(o.table);
  const before = Buffer.from(flash);
  const est = esptoolStandIn(flash, o.esptool || {});
  const page = loadPage(fakePort(), o.settings || {}, { confirm: () => o.confirm !== false });
  page.ctx.__mod = est.mod;
  page.q('loadEsptool=async function(){return __mod}');
  page.ctx.__img = img.buffer.slice(img.byteOffset, img.byteOffset + img.byteLength);
  let result;
  try { result = await page.q('doFlash(__img)'); } catch (e) { check(false, 'doFlash ran without an error (' + e.message + ')'); }
  body({ img, flash, before, seen: est.seen, page, result });
}

(async () => {
  const img0 = mergedImage();
  const hole = [0x9000, 0xF000];   // the settings partition in the image's table
  const allButHole = (flash, img) => same(flash, img, 0, hole[0]) && same(flash, img, hole[1], img.length);

  await flashCase('1. keep settings (the default) on a device with the current layout', { table: T_NOW }, ({ img, flash, before, seen, page, result }) => {
    check(result === true, 'the flash completes');
    check(same(flash, before, hole[0], hole[1]), 'the settings partition (0x9000 to 0xEFFF) is as it was, byte for byte');
    check(allButHole(flash, img), 'everything else up to the end of the image is the new image');
    check(same(flash, before, Math.ceil(img.length / 4096) * 4096, FLASH), 'nothing beyond the image is touched');
    check(JSON.stringify(seen.writes.map(w => w[0])) === '[0,61440]', 'two writes: from 0x0 and from 0xF000 (' + seen.writes.map(w => '0x' + w[0].toString(16)).join(', ') + ')');
    check(JSON.stringify(seen.reads) === '[[32768,3072]]', "the device's partition table was read first");
    check(page.flashLogs.some(l => /Settings partition found at 0x9000 \(24 kB\): it will not be written/.test(l)) && page.flashLogs.some(l => /settings kept/.test(l)), 'the log says so');
    check(page.confirms.length === 0, 'no question asked');
  });

  await flashCase('2. keep settings on a device still on the April layout (two OTA slots)', { table: T_APRIL }, ({ img, flash, before, seen, result }) => {
    check(result === true && same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'new boot loader, table and application; the settings partition untouched');
  });

  await flashCase('3. "Keep settings" unticked', { table: T_NOW, settings: { flash_keep: '0' } }, ({ img, flash, seen, page, result }) => {
    check(result === true && same(flash, img, 0, img.length), 'the whole image is written from 0x0, as before');
    check(flash.subarray(hole[0], hole[1]).every(b => b === 0xFF), 'the settings partition is erased');
    check(seen.reads.length === 0 && seen.writes.length === 1 && seen.writes[0][0] === 0, 'one write, no read of the device');
    check(page.flashLogs.some(l => /settings reset/.test(l)), 'the log says the settings were reset');
  });

  await flashCase('4. keep settings, but the device has its settings somewhere else – and the user says go on', { table: T_ELSEWHERE, confirm: true }, ({ img, flash, seen, page, result }) => {
    check(page.confirms.length === 1 && /settings cannot be kept/.test(page.confirms[0]) && /0x12000/.test(page.confirms[0]), 'the page asks, and says where the device has them');
    check(result === true && same(flash, img, 0, img.length), 'then the whole image is written');
    check(page.flashLogs.some(l => /settings will be reset/.test(l)), 'and the log says the settings are reset');
  });

  await flashCase('5. the same, and the user says no', { table: T_ELSEWHERE, confirm: false }, ({ flash, before, seen, page, result }) => {
    check(result === false && Buffer.compare(flash, before) === 0, 'nothing is written');
    check(seen.writes.length === 0 && seen.resets === 1, 'and the device is restarted into the firmware it has');
    check(page.flashLogs.some(l => /Not flashed/.test(l)), 'the log says it was not flashed');
  });

  await flashCase('6. keep settings on a device that has never been flashed', { table: null }, ({ img, flash, seen, page, result }) => {
    check(result === true && allButHole(flash, img) && flash.subarray(hole[0], hole[1]).every(b => b === 0xFF), 'the image is written around an empty settings partition');
    check(page.confirms.length === 0 && page.flashLogs.some(l => /no partition table: there are no settings to keep/.test(l)), 'no question; the log says there was nothing to keep');
  });

  await flashCase("7. the device's partition table cannot be read", { table: T_NOW, esptool: { readFails: true } }, ({ img, flash, before, page, result }) => {
    check(result === true && same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'the settings partition is left unwritten all the same');
    check(page.flashLogs.some(l => /Could not read the device's partition table/.test(l)), 'and the log says the table could not be read');
  });

  await flashCase('8. the boot loader does not answer at the faster speed', { table: T_NOW, esptool: { noAnswerAbove: 115200 } }, ({ img, flash, before, seen, page, result }) => {
    check(result === true && JSON.stringify(seen.speeds) === '[460800,115200]', 'tried at 460800, done at 115200');
    check(same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'settings kept');
    check(page.store.get('flash_fast_failed') === '460800', 'the faster speed is remembered as not working');
  });

  await flashCase('9. a write fails at the faster speed', { table: T_NOW, esptool: { writeFailsAbove: 115200 } }, ({ img, flash, before, seen, result }) => {
    check(result === true && JSON.stringify(seen.speeds) === '[460800,115200]', 'the whole flash is done again at 115200');
    check(same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'the result is complete, settings kept');
    check(seen.reads.length === 1, "the device's table is not read a second time");
  });

  {
    const app = new Uint8Array(Buffer.concat([Buffer.from([0xE9]), noise(200000, 5)]));   // an application image, not a merged one
    await flashCase('10. keep settings with a file that is not a merged image', { table: T_NOW, image: app }, ({ flash, before, seen, page, result }) => {
      check(result === false && Buffer.compare(flash, before) === 0 && seen.speeds.length === 0, 'refused before the port is touched');
      check(page.flashLogs.some(l => /not a merged image/.test(l)), 'the log says why');
    });
    await flashCase('11. the same file with "Keep settings" unticked, and the user says no to the warning', { table: T_NOW, image: app, settings: { flash_keep: '0' }, confirm: false }, ({ flash, before, seen, page, result }) => {
      check(page.confirms.length === 1 && /does not look like a merged image/.test(page.confirms[0]), 'the page warns that it does not look like a merged image');
      check(result === false && Buffer.compare(flash, before) === 0 && seen.speeds.length === 0, 'and writes nothing');
    });
  }

  await flashCase('12. the setting is kept between visits', { table: T_NOW }, ({ page }) => {
    check(page.q('flashKeepWanted()') === true, 'on by default');
    page.q('flashSetKeep(false)');
    check(page.store.get('flash_keep') === '0' && page.q('flashKeepWanted()') === false, 'off once unticked, and stored');
  });

  console.log(process.env.IMAGE ? '(image: ' + process.env.IMAGE + ', ' + img0.length + ' bytes)' : '(image: made up, ' + img0.length + ' bytes)');
  console.log();
  if (fails.length) { console.log('check_page_flash: ' + fails.length + ' FAILED'); process.exit(1); }
  console.log('check_page_flash: ok');
  process.exit(0);
})();
