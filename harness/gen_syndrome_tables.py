#!/usr/bin/env python3
"""gen_syndrome_tables.py - generate mode_s_syndromes.h for adsb-scope.

Emits, as const C tables:
  mode_s_crc24_table[256]   byte-wise CRC-24 (poly 0xFFF409, init 0)
  mode_s_syn1_112[112]      sorted (crc<<8 | bitpos), single-bit errors, 112-bit frames
  mode_s_syn1_56[56]        sorted (crc<<8 | bitpos), single-bit errors, 56-bit frames
  mode_s_syn2_112_a[6216]   sorted (crc<<8 | firstpos), double-bit errors, 112-bit frames
  mode_s_syn2_112_b[6216]   parallel second-error positions

Table layout and packing follow more1090 (Daniel Estevez, MIT OR
Apache-2.0), src/mode_s.rs; re-derived independently here.

Built-in verification - the script exits non-zero if any check fails:
  1. Known vector: CRC24("8D406B902015A678D4D220") == 0xAA4BDA, and the
     CRC over message+parity == 0 (mode-s.org reference vector).
  2. Equivalence with the firmware's XOR-table convention: for random
     frames, the byte-wise CRC over the WHOLE frame equals
     CRC24(stored_crc XOR mode_s_checksum(frame)) taken as a 3-byte
     value, and is 0 exactly when stored_crc == mode_s_checksum(frame).
  3. Uniqueness: all 1-bit and 2-bit syndromes over 112-bit frames are
     jointly distinct and nonzero (so <=2-bit correction is exact, never
     ambiguous); the 56-bit single-bit syndromes are distinct and nonzero.
  4. Round-trip: random valid frames with injected 1/2-bit errors are
     located exactly via the emitted tables.

usage: gen_syndrome_tables.py [output_header]
       default: ../main/examples/adsb_receiver/mode_s_syndromes.h
"""

import bisect
import os
import random
import sys

POLY = 0xFFF409

# The firmware's 112-entry XOR table (mode_s_checksum_table in mode-s.c),
# reproduced here for the equivalence proof (check 2).
FW_TABLE = [
    0x3935EA, 0x1C9AF5, 0xF1B77E, 0x78DBBF, 0xC397DB, 0x9E31E9, 0xB0E2F0, 0x587178,
    0x2C38BC, 0x161C5E, 0x0B0E2F, 0xFA7D13, 0x82C48D, 0xBE9842, 0x5F4C21, 0xD05C14,
    0x682E0A, 0x341705, 0xE5F186, 0x72F8C3, 0xC68665, 0x9CB936, 0x4E5C9B, 0xD8D449,
    0x939020, 0x49C810, 0x24E408, 0x127204, 0x093902, 0x049C81, 0xFDB444, 0x7EDA22,
    0x3F6D11, 0xE04C8C, 0x702646, 0x381323, 0xE3F395, 0x8E03CE, 0x4701E7, 0xDC7AF7,
    0x91C77F, 0xB719BB, 0xA476D9, 0xADC168, 0x56E0B4, 0x2B705A, 0x15B82D, 0xF52612,
    0x7A9309, 0xC2B380, 0x6159C0, 0x30ACE0, 0x185670, 0x0C2B38, 0x06159C, 0x030ACE,
    0x018567, 0xFF38B7, 0x80665F, 0xBFC92B, 0xA01E91, 0xAFF54C, 0x57FAA6, 0x2BFD53,
    0xEA04AD, 0x8AF852, 0x457C29, 0xDD4410, 0x6EA208, 0x375104, 0x1BA882, 0x0DD441,
    0xF91024, 0x7C8812, 0x3E4409, 0xE0D800, 0x706C00, 0x383600, 0x1C1B00, 0x0E0D80,
    0x0706C0, 0x038360, 0x01C1B0, 0x00E0D8, 0x00706C, 0x003836, 0x001C1B, 0xFFF409,
] + [0] * 24


def crc24_bit(data: bytes) -> int:
    """Reference bit-at-a-time CRC-24 (poly 0xFFF409, init 0)."""
    crc = 0
    for b in data:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= POLY
        crc &= 0xFFFFFF
    return crc


def build_byte_table():
    tbl = []
    for b in range(256):
        crc = b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= POLY
        tbl.append(crc & 0xFFFFFF)
    return tbl


BYTE_TABLE = build_byte_table()


def crc24(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFF) ^ BYTE_TABLE[((crc >> 16) ^ b) & 0xFF]
    return crc


def fw_checksum(msg: bytes, bits: int) -> int:
    """Firmware mode_s_checksum(): XOR of table entries for set bits."""
    offset = 0 if bits == 112 else (112 - 56)
    crc = 0
    for j in range(bits):
        if msg[j // 8] & (1 << (7 - (j % 8))):
            crc ^= FW_TABLE[j + offset]
    return crc


def pattern_crc(bits: int, *positions) -> int:
    buf = bytearray(bits // 8)
    for j in positions:
        buf[j // 8] ^= 1 << (7 - (j % 8))
    return crc24(bytes(buf))


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "main", "examples", "adsb_receiver", "mode_s_syndromes.h")

    # -- Check 1: known vector ---------------------------------------------
    v = bytes.fromhex("8D406B902015A678D4D220")
    if crc24(v) != 0xAA4BDA or crc24_bit(v) != 0xAA4BDA:
        fail(f"known vector CRC mismatch: {crc24(v):06X}")
    if crc24(v + bytes.fromhex("AA4BDA")) != 0:
        fail("known vector full-frame CRC != 0")

    # -- Check 2: equivalence with the firmware convention -----------------
    rng = random.Random(0x1090)
    for bits in (112, 56):
        nb = bits // 8
        for _ in range(500):
            msg = bytes(rng.randrange(256) for _ in range(nb))
            stored = (msg[nb - 3] << 16) | (msg[nb - 2] << 8) | msg[nb - 1]
            raw = stored ^ fw_checksum(msg, bits)
            raw3 = bytes([(raw >> 16) & 0xFF, (raw >> 8) & 0xFF, raw & 0xFF])
            if crc24(msg) != crc24(raw3):
                fail(f"equivalence failed for bits={bits}")
            if (crc24(msg) == 0) != (raw == 0):
                fail(f"validity equivalence failed for bits={bits}")

    # -- Build tables --------------------------------------------------------
    syn1_112 = sorted((pattern_crc(112, j) << 8) | j for j in range(112))
    syn1_56 = sorted((pattern_crc(56, j) << 8) | j for j in range(56))
    pairs = sorted((((pattern_crc(112, j, k) << 8) | j, k)
                    for j in range(112) for k in range(j + 1, 112)),
                   key=lambda t: t[0])
    syn2_a = [a for a, _ in pairs]
    syn2_b = [b for _, b in pairs]
    if len(syn2_a) != 112 * 111 // 2:
        fail("unexpected double-bit table length")

    # -- Check 3: uniqueness / nonzero --------------------------------------
    joint = [x >> 8 for x in syn1_112] + [x >> 8 for x in syn2_a]
    if len(set(joint)) != len(joint) or 0 in joint:
        fail("1-bit/2-bit syndromes over 112 bits are not jointly unique and nonzero")
    s56 = [x >> 8 for x in syn1_56]
    if len(set(s56)) != len(s56) or 0 in s56:
        fail("56-bit single-bit syndromes are not unique and nonzero")

    # -- Check 4: round-trip through the emitted tables ---------------------
    def lookup1(tbl, syn):
        i = bisect.bisect_left(tbl, syn << 8)
        return tbl[i] & 0xFF if i < len(tbl) and (tbl[i] >> 8) == syn else None

    def lookup2(syn):
        i = bisect.bisect_left(syn2_a, syn << 8)
        if i < len(syn2_a) and (syn2_a[i] >> 8) == syn:
            return syn2_a[i] & 0xFF, syn2_b[i]
        return None

    for _ in range(300):
        msg = bytearray(rng.randrange(256) for _ in range(14))
        c = crc24(bytes(msg[:11]))
        msg[11], msg[12], msg[13] = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
        orig = bytes(msg)
        j = rng.randrange(112)
        bad = bytearray(orig)
        bad[j // 8] ^= 1 << (7 - (j % 8))
        if lookup1(syn1_112, crc24(bytes(bad))) != j:
            fail("1-bit round-trip failed")
        lo, hi = sorted(rng.sample(range(112), 2))
        bad = bytearray(orig)
        bad[lo // 8] ^= 1 << (7 - (lo % 8))
        bad[hi // 8] ^= 1 << (7 - (hi % 8))
        if lookup2(crc24(bytes(bad))) != (lo, hi):
            fail("2-bit round-trip failed")

    # -- Emit header ----------------------------------------------------------
    def fmt_u32(vals, per=8):
        return "\n".join("    " + ", ".join(f"0x{v:08X}" for v in vals[i:i + per]) + ","
                         for i in range(0, len(vals), per))

    def fmt_u8(vals, per=16):
        return "\n".join("    " + ", ".join(f"{v:3d}" for v in vals[i:i + per]) + ","
                         for i in range(0, len(vals), per))

    hdr = f"""// mode_s_syndromes.h - GENERATED by harness/gen_syndrome_tables.py. Do not edit.
//
// Byte-wise CRC-24 (poly 0xFFF409, init 0) and exact syndrome tables for
// <=2-bit error correction of Mode-S frames. Table packing follows
// more1090 (Daniel Estevez, MIT OR Apache-2.0): sorted (crc<<8 | bitpos)
// for binary search.
//
// Properties PROVEN by the generator at emit time:
//  - all 1-bit and 2-bit syndromes over 112-bit frames are jointly unique
//    and nonzero, so a table hit identifies the error pattern exactly;
//  - likewise the 56-bit single-bit syndromes;
//  - the byte-wise CRC over the ENTIRE frame (data + parity) is 0 exactly
//    when stored_crc == mode_s_checksum(frame). When nonzero it is the
//    error syndrome in "full-frame CRC space": CRC24 of the 3-byte value
//    (stored_crc XOR mode_s_checksum(frame)). The tables below are built
//    in that same space.
#pragma once
#include <stdint.h>

#define MODE_S_SYN2_112_LEN 6216

static const uint32_t mode_s_crc24_table[256] = {{
{fmt_u32(BYTE_TABLE)}
}};

static const uint32_t mode_s_syn1_112[112] = {{
{fmt_u32(syn1_112)}
}};

// Syndrome of a single-bit error at each bit position of a 112-bit frame,
// indexed by position (the same values as mode_s_syn1_112, unsorted).
static const uint32_t mode_s_syn_bit_112[112] = {{
{fmt_u32([pattern_crc(112, j) for j in range(112)])}
}};

static const uint32_t mode_s_syn1_56[56] = {{
{fmt_u32(syn1_56)}
}};

static const uint32_t mode_s_syn2_112_a[MODE_S_SYN2_112_LEN] = {{
{fmt_u32(syn2_a)}
}};

static const uint8_t mode_s_syn2_112_b[MODE_S_SYN2_112_LEN] = {{
{fmt_u8(syn2_b)}
}};
"""
    with open(out, "w") as f:
        f.write(hdr)
    const_bytes = 256 * 4 + 112 * 4 + 112 * 4 + 56 * 4 + 6216 * 4 + 6216
    print(f"OK: all checks passed; wrote {os.path.normpath(out)} "
          f"({os.path.getsize(out)} bytes of source; {const_bytes} bytes of const data)")


if __name__ == "__main__":
    main()
