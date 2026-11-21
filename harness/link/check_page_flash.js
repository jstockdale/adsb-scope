#!/usr/bin/env node
// check_page_flash.js - the web app's flashing code (doFlash in
// adsb_scope.htm, as it is) with the real esptool-js, against an emulated
// chip (fake_esp.js: reset lines, ROM loader, flasher stub, flash memory).
//
// What it is for:
//   - "keep settings": is the settings partition left alone, and only when it
//     should be? (read of the device's table, the two-piece write, the
//     question when the settings are somewhere else)
//   - the speeds: no answer or a failed write at the faster speed, then 115200
//   - that the page gets along with esptool-js as it is, not as a stand-in
//     has it: a first version of this test used a stand-in, which had a
//     hardReset() and a main() that gives up - esptool-js 0.5.4 has neither.
//
// It needs the esptool-js bundle the page loads (unpkg.com/esptool-js@0.5.4/
// bundle.js), which is not in this repository:
//     make esptool-js          fetches it with npm into harness/.cache
//     ESPTOOL_JS=<bundle.js>   uses another copy
// Without it the test says so and is skipped. IMAGE=<merged .bin> runs the
// cases on a real image instead of a made-up one; ONLY=1,8 runs some.
//
// usage: node check_page_flash.js      (a minute and a half, most of it in the
//        cases where a loader does not answer; part of make check.
//        make check-quick runs the cases that take no waiting, ten seconds)
'use strict';
const fs = require('fs'), os = require('os'), path = require('path'), crypto = require('crypto'), { pathToFileURL } = require('url');
const { loadPage } = require('./page_env.js');
const { FakeChip, fakePort } = require('./fake_esp.js');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const fails = [];
function check(cond, what) { console.log((cond ? '  ok    ' : '  FAIL  ') + what); if (!cond) fails.push(what); }

const PIN = { version: '0.5.4', sha256: '2a896d5e520ea9b6ea9900223c2460df797c3b2287f441daa4e50168a50b17e6' };
const BUNDLE = process.env.ESPTOOL_JS || path.join(__dirname, '..', '.cache', 'esptool-js-' + PIN.version, 'package', 'bundle.js');

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
function same(a, b, lo, hi) { return Buffer.compare(Buffer.from(a.buffer, a.byteOffset + lo, hi - lo), Buffer.from(b.buffer, b.byteOffset + lo, hi - lo)) === 0; }
const pad4 = n => (n + 3) & ~3;
const hex = n => '0x' + n.toString(16);

// esptool-js reports every read that times out on console.error ("Error reading
// from serial port: Error: Read timeout exceeded") - dozens while a loader does
// not answer. Counted here, not printed.
let readTimeouts = 0;
const consoleError = console.error;
console.error = (...a) => { if (/Read timeout exceeded/.test(a.map(String).join(' '))) readTimeouts++; else consoleError(...a); };

let esptool = null;
async function flashCase(title, o, body) {
  if (process.env.ONLY && !process.env.ONLY.split(',').includes(title.split('.')[0])) return;
  console.log(title);
  const img = o.image || mergedImage();
  const flash = device(o.table);
  const before = Buffer.from(flash);
  const chip = new FakeChip(flash, o.chip || {});
  const port = fakePort(chip);
  const page = loadPage(port, o.settings || {}, { confirm: () => o.confirm !== false });
  page.ctx.__mod = esptool;
  page.q('loadEsptool=async function(){return __mod}');
  page.ctx.__img = img.buffer.slice(img.byteOffset, img.byteOffset + img.byteLength);
  const t0 = Date.now();
  let result;
  try { result = await page.q('doFlash(__img)'); } catch (e) { check(false, 'doFlash ran without an error (' + e.message + ')'); }
  const secs = (Date.now() - t0) / 1000;
  await sleep(150);                       // the restart at the end is a pulse of 100 ms
  port.kill();                            // the page reconnects three seconds after a flash: not part of this
  if (chip.seen.errors.length) check(false, 'the chip saw no protocol errors (' + chip.seen.errors.join('; ') + ')');
  body({ img, flash, before, chip, port, page, result, secs });
}

(async () => {
  if (!fs.existsSync(BUNDLE)) {
    console.log("SKIPPED: link/check_page_flash.js (the web app's flashing with the real esptool-js against an emulated chip) - the esptool-js bundle is not here.\n" +
                '         "make esptool-js" fetches it (npm, network); or ESPTOOL_JS=<path to esptool-js ' + PIN.version + "'s bundle.js>");
    return;
  }
  const sum = crypto.createHash('sha256').update(fs.readFileSync(BUNDLE)).digest('hex');
  const tmp = path.join(os.tmpdir(), 'esptool-js-' + process.pid + '.mjs');
  fs.copyFileSync(BUNDLE, tmp);
  try { esptool = await import(pathToFileURL(tmp).href); } finally { fs.unlinkSync(tmp); }
  console.log('esptool-js: ' + (sum === PIN.sha256 ? PIN.version + ' (the bundle the page loads, sha256 ' + sum.slice(0, 16) + '...)' : 'NOT the pinned ' + PIN.version + ' bundle (sha256 ' + sum + ')'));
  check(typeof esptool.ESPLoader === 'function' && typeof esptool.Transport === 'function', 'the bundle has ESPLoader and Transport');
  check(typeof esptool.ESPLoader.prototype.hardReset === 'undefined', 'and no ESPLoader.hardReset() - which the page called until 1.0.10');

  const img0 = mergedImage();
  const hole = [0x9000, 0xF000];   // the settings partition in the image's table
  const allButHole = (flash, img) => same(flash, img, 0, hole[0]) && same(flash, img, hole[1], img.length);
  const twoPieces = img => JSON.stringify([[0, hole[0]], [hole[1], pad4(img.length - hole[1])]]);

  await flashCase('1. keep settings (the default) on a device with the current layout', { table: T_NOW }, ({ img, flash, before, chip, port, page, result }) => {
    check(result === true, 'the flash completes');
    check(same(flash, before, hole[0], hole[1]), 'the settings partition (0x9000 to 0xEFFF) is as it was, byte for byte');
    check(allButHole(flash, img), 'everything else up to the end of the image is the new image');
    check(same(flash, before, Math.ceil(img.length / 4096) * 4096, FLASH), 'nothing beyond the image is touched');
    check(JSON.stringify(chip.seen.begins) === twoPieces(img), 'the stub was given two writes: ' + chip.seen.begins.map(b => hex(b[0]) + ' + ' + hex(b[1])).join(', '));
    check(JSON.stringify(chip.seen.reads) === '[[32768,3072]]', "the device's partition table was read first (0x8000, 3072 bytes)");
    check(page.flashLogs.some(l => /Settings partition found at 0x9000 \(24 kB\): it will not be written/.test(l)) && page.flashLogs.some(l => /settings kept/.test(l)), 'the log says so');
    check(page.confirms.length === 0, 'no question asked');
    check(JSON.stringify(port.seen.opens) === '[460800]' && chip.seen.changeBaud === 0, 'the port was opened once, at 460800, and no speed change was asked of the loader');
    check(chip.seen.stubBytes > 1000 && chip.seen.unknown.length === 0, "esptool-js loaded its stub (" + chip.seen.stubBytes + ' bytes) and sent nothing the chip does not know');
    check(chip.seen.resets.join(',') === 'download,firmware' && chip.mode === 'firmware', 'reset into the loader once, and restarted into the firmware at the end (' + chip.seen.resets.join(', ') + ')');
    check(!page.flashLogs.some(l => /Auto-reset failed/.test(l)), 'without "Auto-reset failed"');
  });

  await flashCase('2. keep settings on a device still on the April layout (two OTA slots)', { table: T_APRIL }, ({ img, flash, before, result }) => {
    check(result === true && same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'new boot loader, table and application; the settings partition untouched');
  });

  await flashCase('3. "Keep settings" unticked', { table: T_NOW, settings: { flash_keep: '0' } }, ({ img, flash, chip, page, result }) => {
    check(result === true && same(flash, img, 0, img.length), 'the whole image is written from 0x0, as before');
    check(flash.subarray(hole[0], hole[1]).every(b => b === 0xFF), 'the settings partition is erased');
    check(chip.seen.reads.length === 0 && JSON.stringify(chip.seen.begins) === JSON.stringify([[0, pad4(img.length)]]), 'one write, no read of the device');
    check(page.flashLogs.some(l => /settings reset/.test(l)), 'the log says the settings were reset');
  });

  await flashCase('4. keep settings, but the device has its settings somewhere else - and the user says go on', { table: T_ELSEWHERE }, ({ img, flash, chip, page, result }) => {
    check(page.confirms.length === 1 && /0x12000/.test(page.confirms[0]) && /0x9000/.test(page.confirms[0]), 'the page asks, and says where the device has them');
    check(result === true && same(flash, img, 0, img.length) && chip.seen.begins.length === 1, 'then the whole image is written');
    check(page.flashLogs.some(l => /settings will be reset/.test(l)) && page.flashLogs.some(l => /settings reset/.test(l)), 'and the log says the settings are reset');
  });

  await flashCase('5. the same, and the user says no', { table: T_ELSEWHERE, confirm: false }, ({ flash, before, chip, page, result }) => {
    check(result === false && same(flash, before, 0, FLASH) && chip.seen.begins.length === 0, 'nothing is written');
    check(chip.seen.resets.join(',') === 'download,firmware' && chip.mode === 'firmware', 'and the device is restarted into the firmware it has');
    check(page.flashLogs.some(l => /Not flashed/.test(l)), 'the log says it was not flashed');
  });

  await flashCase('6. keep settings on a device that has never been flashed', { table: null }, ({ img, flash, chip, page, result }) => {
    check(result === true && allButHole(flash, img) && flash.subarray(hole[0], hole[1]).every(b => b === 0xFF), 'the image is written around an empty settings partition');
    check(page.confirms.length === 0 && page.flashLogs.some(l => /no partition table: there are no settings to keep/.test(l)), 'no question; the log says there was nothing to keep');
  });

  await flashCase("7. the device's partition table cannot be read (the stub does not answer the read)", { table: T_NOW, chip: { readFlashSilent: true } }, ({ img, flash, before, page, result }) => {
    check(result === true && same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'the settings partition is left unwritten all the same');
    check(page.flashLogs.some(l => /Could not read the device's partition table/.test(l)), 'and the log says the table could not be read');
  });

  await flashCase('8. the ROM loader does not answer at the faster speed', { table: T_NOW, chip: { romDeafAbove: 115200 } }, ({ img, flash, before, port, page, result, secs }) => {
    check(result === true && JSON.stringify(port.seen.opens) === '[460800,115200]', 'tried at 460800, done at 115200 (' + secs.toFixed(0) + ' s in all)');
    check(page.flashLogs.some(l => /No answer from the bootloader at 460800 baud .* again at 115200/.test(l)), 'the page gave up on 460800 by itself and says so');
    check(same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'settings kept');
    check(page.store.get('flash_fast_failed') === '460800', 'the faster speed is remembered as not working');
  });

  await flashCase('9. a write stalls at the faster speed', { table: T_NOW, chip: { stallWriteAbove: 115200 } }, ({ img, flash, before, chip, port, page, result }) => {
    check(result === true && JSON.stringify(port.seen.opens) === '[460800,115200]', 'the whole flash is done again at 115200');
    check(page.flashLogs.some(l => /Write failed at 460800 baud/.test(l)), 'the log says the write failed at 460800');
    check(same(flash, before, hole[0], hole[1]) && allButHole(flash, img), 'the result is complete, settings kept');
    check(chip.seen.reads.length === 1, "the device's table is not read a second time (the first attempt may have replaced it)");
    check(!page.store.has('flash_fast_failed'), 'the faster speed is not written off: the loader did answer at it');
  });

  const appOnly = new Uint8Array(noise(200000, 5)); appOnly[0] = 0xE9;
  await flashCase('10. keep settings with a file that is not a merged image', { table: T_NOW, image: appOnly }, ({ flash, before, port, page, result }) => {
    check(result === false && same(flash, before, 0, FLASH) && port.seen.opens.length === 0, 'refused before the port is opened');
    check(page.flashLogs.some(l => /not a merged image/.test(l)), 'the log says why');
  });

  await flashCase('11. the same file with "Keep settings" unticked, and the user says no to the warning', { table: T_NOW, image: appOnly, settings: { flash_keep: '0' }, confirm: false }, ({ flash, before, port, page, result }) => {
    check(page.confirms.length === 1 && /does not look like a merged image/.test(page.confirms[0]), 'the page warns that it does not look like a merged image');
    check(result === false && same(flash, before, 0, FLASH) && port.seen.opens.length === 0, 'and writes nothing');
  });

  await flashCase('12. a device that does not go into its loader at all (boot pin not reached)', { table: T_NOW, chip: { noDownload: true } }, ({ flash, before, chip, port, page, result, secs }) => {
    check(result === false && same(flash, before, 0, FLASH), 'nothing is written');
    check(secs < 120, 'the page gives up by itself: ' + secs.toFixed(0) + ' s, ' + chip.seen.resets.length + ' resets (esptool-js 0.5.4\'s main() would make 460800 attempts)');
    check(JSON.stringify(port.seen.opens) === '[460800,115200]' && page.flashLogs.some(l => /Bootloader connection failed/.test(l)), 'after trying both speeds, with "Bootloader connection failed"');
    check(!page.store.has('flash_fast_failed'), 'and the faster speed is not blamed for it');
  });

  if (!process.env.ONLY) {
    console.log('13. the setting is kept between visits');
    const p1 = loadPage(fakePort(new FakeChip(device(T_NOW))), {}, {});
    check(p1.q('flashKeepWanted()') === true, 'on by default');
    p1.q('flashSetKeep(false)');
    check(p1.q('flashKeepWanted()') === false && p1.store.get('flash_keep') === '0', 'off once unticked, and stored');
  }

  console.log('(image: ' + (process.env.IMAGE ? process.env.IMAGE + ', ' : 'made up, ') + img0.length + ' bytes; ' + readTimeouts + ' read timeouts reported by esptool-js while loaders did not answer)');
  console.log(fails.length ? '\ncheck_page_flash: ' + fails.length + ' FAILED' : '\ncheck_page_flash: ok');
  process.exit(fails.length ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
