// fake_esp.js - an ESP32-P4 as esptool-js sees it through a Web Serial port:
// the reset and boot-mode lines, the ROM loader's and the flasher stub's
// serial protocol (SLIP frames), and a flash memory. Enough of each for the
// real esptool-js to reset the chip into its loader, sync, identify it, load
// its stub, read flash and write compressed images - so that the web app's
// flashing code can be run against the library it actually uses.
//
// What it copies from the real thing:
//   lines    EN is low while RTS is asserted and DTR is not; the boot pin is
//            low while DTR is asserted and RTS is not; the boot pin is sampled
//            a moment after EN rises (the capacitor on EN). A reset prints the
//            ROM's banner.
//   ROM      SYNC (eight answers, value not 0), READ_REG, MEM_BEGIN/DATA/END;
//            after MEM_END the "stub" starts and says OHAI.
//   stub     SYNC (value 0), READ_REG, READ_FLASH (data frames, the host's
//            4-byte acknowledgements, then the 16-byte MD5 that esptool-js
//            0.5.4 leaves unread), FLASH_DEFL_BEGIN/DATA/END, FLASH_BEGIN.
//            FLASH_*_BEGIN erases as stub_write_flash.c does: the sectors from
//            the first to the last of the size it is given.
// What it does not: timing (it answers at once, at any speed unless told
// otherwise), flash encryption, anything esptool-js does not send here.
'use strict';
const zlib = require('zlib'), crypto = require('crypto');

const SECTOR = 4096;
const le32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;

class FakeChip {
  // flash: Buffer. opt.romDeafAbove: the ROM loader does not answer when the port is faster than this.
  // opt.stallWriteAbove: the stub stops answering in the third block of a write when the port is faster than this.
  // opt.readFlashSilent: the stub does not answer READ_FLASH. opt.noDownload: the boot pin is not connected.
  constructor(flash, opt) {
    this.flash = flash; this.opt = opt || {};
    this.mode = 'firmware';                 // 'firmware' | 'held' | 'rom' | 'stub'
    this.dtr = false; this.rts = false; this.baud = 0;
    this.send = () => {};
    this.frame = null; this.esc = false; this.rd = null; this.defl = null;
    this.seen = { resets: [], reads: [], begins: [], writes: [], flashBegins: [], ops: {}, unknown: [], errors: [], stubBytes: 0, changeBaud: 0 };
  }

  // ---- the two lines ----
  signals(s) {
    const enWas = this.rts && !this.dtr;
    if ('dataTerminalReady' in s) this.dtr = !!s.dataTerminalReady;
    if ('requestToSend' in s) this.rts = !!s.requestToSend;
    const enNow = this.rts && !this.dtr;
    if (enNow && !enWas) { this.mode = 'held'; this.rd = null; this.defl = null; this.frame = null; }
    if (!enNow && enWas) setTimeout(() => this.boot(this.dtr && !this.rts && !this.opt.noDownload), 2);
  }
  boot(download) {
    this.seen.resets.push(download ? 'download' : 'firmware');
    this.rd = null; this.defl = null; this.frame = null; this.esc = false;
    this.mode = download ? 'rom' : 'firmware';
    this.send(Buffer.from(download
      ? 'ESP-ROM:esp32p4-eco2-20240710\r\nBuild:Jul 10 2024\r\nrst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))\r\nwaiting for download\r\n'
      : 'ESP-ROM:esp32p4-eco2-20240710\r\nBuild:Jul 10 2024\r\nrst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)\r\n'));
  }

  // ---- bytes from the host ----
  fromHost(chunk) {
    if (this.mode !== 'rom' && this.mode !== 'stub') return;
    if (this.mode === 'rom' && this.opt.romDeafAbove && this.baud > this.opt.romDeafAbove) return;
    for (const b of chunk) {
      if (this.frame === null) { if (b === 0xC0) this.frame = []; continue; }
      if (this.esc) { this.esc = false; this.frame.push(b === 0xDC ? 0xC0 : b === 0xDD ? 0xDB : b); continue; }
      if (b === 0xDB) { this.esc = true; continue; }
      if (b === 0xC0) { const f = this.frame; this.frame = f.length ? null : []; if (f.length) this.onFrame(Buffer.from(f)); continue; }
      this.frame.push(b);
    }
  }
  sendFrame(buf) {
    const out = [0xC0];
    for (const b of buf) { if (b === 0xC0) out.push(0xDB, 0xDC); else if (b === 0xDB) out.push(0xDB, 0xDD); else out.push(b); }
    out.push(0xC0);
    this.send(Buffer.from(out));
  }
  respond(op, val, err) {
    const status = this.mode === 'stub' ? [err ? 1 : 0, err || 0] : [err ? 1 : 0, err || 0, 0, 0];
    const p = Buffer.alloc(8 + status.length);
    p[0] = 1; p[1] = op; p.writeUInt16LE(status.length, 2); p.writeUInt32LE((val || 0) >>> 0, 4); Buffer.from(status).copy(p, 8);
    this.sendFrame(p);
  }
  reg(addr) {
    switch (addr) {
      case 0x40001000: return 0x0addbad0;          // the value esptool-js knows an ESP32-P4 by
      case 0x5012d04c: return 0x00000013;          // eFuse block 1, word 2: revision v1.3, package 0
      case 0x5012d044: return 0xf9f71234;          // MAC
      case 0x5012d048: return 0x00006055;
      default: return 0;
    }
  }
  erase(offset, size) {                            // handle_flash_begin() in the stub's stub_write_flash.c
    const first = Math.floor(offset / SECTOR), n = Math.floor(((offset % SECTOR) + size + SECTOR - 1) / SECTOR);
    this.flash.fill(0xFF, first * SECTOR, (first + n) * SECTOR);
  }

  onFrame(f) {
    if (this.rd && f.length === 4) return this.readAck(le32(f, 0));
    if (f.length < 8 || f[0] !== 0) return;
    const op = f[1], len = f[2] | f[3] << 8, chk = le32(f, 4), d = f.subarray(8, 8 + len);
    const stub = this.mode === 'stub';
    this.seen.ops[op] = (this.seen.ops[op] || 0) + 1;
    switch (op) {
      case 0x08: for (let i = 0; i < 8; i++) this.respond(op, stub ? 0 : 0x20120707); return;       // SYNC
      case 0x0a: return this.respond(op, this.reg(le32(d, 0)));                                    // READ_REG
      case 0x05: return this.respond(op);                                                          // MEM_BEGIN
      case 0x07: this.seen.stubBytes += le32(d, 0); return this.respond(op);                       // MEM_DATA
      case 0x06:                                                                                   // MEM_END: run it
        this.respond(op);
        if (!stub) { this.mode = 'stub'; this.sendFrame(Buffer.from('OHAI')); }
        return;
      case 0x0d: case 0x0b: return this.respond(op);                                               // SPI_ATTACH, SPI_SET_PARAMS
      case 0x0f: this.seen.changeBaud++; return this.respond(op);                                  // CHANGE_BAUDRATE
    }
    if (!stub) { this.seen.unknown.push(op); return this.respond(op, 0, 0x05); }                   // the ROM: invalid message
    switch (op) {
      case 0xd2: {                                                                                 // READ_FLASH
        if (this.opt.readFlashSilent) return;
        const addr = le32(d, 0), size = le32(d, 4), block = le32(d, 8), maxIn = le32(d, 12);
        this.seen.reads.push([addr, size]);
        this.respond(op);
        this.rd = { addr, size, block, maxIn, sent: 0, acked: 0, md5: crypto.createHash('md5') };
        return this.readPump();
      }
      case 0x10: {                                                                                 // FLASH_DEFL_BEGIN
        const size = le32(d, 0), nblocks = le32(d, 4), offset = le32(d, 12);
        this.erase(offset, size);
        this.defl = { offset, size, nblocks, seq: 0, parts: [] };
        this.seen.begins.push([offset, size]);
        return this.respond(op);
      }
      case 0x11: {                                                                                 // FLASH_DEFL_DATA
        const dlen = le32(d, 0), seq = le32(d, 4), payload = d.subarray(16, 16 + dlen), w = this.defl;
        if (!w) { this.seen.errors.push('data without begin'); return this.respond(op, 0, 0x06); }
        if (this.opt.stallWriteAbove && this.baud > this.opt.stallWriteAbove && seq >= 2) return;   // the line gave out
        let x = 0xEF; for (const b of payload) x ^= b;
        if (x !== (chk & 0xFF)) { this.seen.errors.push('checksum of block ' + seq); return this.respond(op, 0, 0x07); }
        if (seq !== w.seq) this.seen.errors.push('block ' + seq + ' where ' + w.seq + ' was due');
        w.parts.push(Buffer.from(payload)); w.seq++;
        if (w.seq === w.nblocks) {
          let data = null;
          try { data = zlib.inflateSync(Buffer.concat(w.parts)); } catch (e) { this.seen.errors.push('inflate: ' + e.message); }
          if (data) {
            if (data.length !== w.size) this.seen.errors.push('inflated to ' + data.length + ', announced ' + w.size);
            data.copy(this.flash, w.offset);
            this.seen.writes.push([w.offset, data.length, this.baud]);
          }
          this.defl = null;
        }
        return this.respond(op);
      }
      case 0x02: {                                                                                 // FLASH_BEGIN
        const size = le32(d, 0), offset = le32(d, 12);
        this.erase(offset, size);
        this.seen.flashBegins.push([offset, size]);
        return this.respond(op);
      }
      case 0x12: case 0x04:                                                                        // FLASH_DEFL_END, FLASH_END
        this.respond(op);
        if (le32(d, 0) === 0) this.boot(false);
        return;
    }
    this.seen.unknown.push(op);
    this.respond(op, 0, 0xFF);
  }
  readPump() {                                                                                     // handle_read_flash() in the stub
    const r = this.rd;
    while (r.sent < r.size && r.sent - r.acked < r.maxIn) {
      const n = Math.min(r.size - r.sent, r.block), buf = this.flash.subarray(r.addr + r.sent, r.addr + r.sent + n);
      r.md5.update(buf); r.sent += n;
      this.sendFrame(buf);
    }
  }
  readAck(n) {
    const r = this.rd; r.acked = n;
    if (r.acked >= r.size) { this.rd = null; this.sendFrame(r.md5.digest()); } else this.readPump();
  }
}

// A Web Serial port with the chip behind it. As in a browser: after its
// reader is cancelled `readable` is a new stream, and close() refuses while a
// stream is locked.
function fakePort(chip) {
  let isOpen = false, dead = false, rs = null, rc = null, ws = null;
  const mk = () => { rs = new ReadableStream({ start(c) { rc = c; }, cancel() { rs = null; rc = null; } }); };
  const port = {
    seen: { opens: [], closes: 0, signals: [] },
    async open(o) {
      if (dead) throw new Error('NetworkError: the device is gone');
      if (isOpen) throw new Error('InvalidStateError: the port is already open');
      isOpen = true; port.seen.opens.push(o.baudRate); chip.baud = o.baudRate; mk();
      ws = new WritableStream({ write(chunk) { chip.fromHost(chunk); } });
    },
    async close() {
      if (!isOpen) throw new Error('InvalidStateError: the port is already closed');
      if ((rs && rs.locked) || (ws && ws.locked)) throw new TypeError('Cannot close the port while a stream is locked');
      isOpen = false; try { if (rc) rc.close(); } catch (e) { /* closed already */ }
      rs = rc = ws = null; port.seen.closes++;
    },
    async setSignals(s) {
      if (!isOpen) throw new Error('InvalidStateError: the port is closed');
      port.seen.signals.push(Object.assign({}, s)); chip.signals(s);
    },
    getInfo() { return { usbVendorId: 0x1a86, usbProductId: 0x55d3 }; },     // a UART bridge, not the chip's own USB-Serial-JTAG
    get readable() { if (!isOpen) return null; if (!rs) mk(); return rs; },
    get writable() { return isOpen ? ws : null; },
    kill() { dead = true; },                                                // the test is done with it: no more opens
  };
  chip.send = bytes => { if (isOpen && rc) { try { rc.enqueue(new Uint8Array(bytes)); } catch (e) { /* stream closed */ } } };
  return port;
}

module.exports = { FakeChip, fakePort, SECTOR };
