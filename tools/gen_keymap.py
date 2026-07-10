#!/usr/bin/env python3
"""Generate a components/core/keymap_*.c table from a Windows .klc file.

The .klc files are exported from Microsoft's keyboard layout DLLs by
https://kbdlayout.info/ (find the layout, then Download -> KLC). They are the
authoritative statement of which key position produces which character, so a
generated table contains no positions from anyone's memory. The files carry a
Microsoft copyright header and are NOT committed to this repo; only the
generated tables (key positions are facts) are.

    python3 tools/gen_keymap.py kbdfr.klc --name fr \
        --out components/core/keymap_fr.c

What it understands:
  - shift states 0 (plain), 1 (Shift), 6 (AltGr), 7 (Shift+AltGr);
    Ctrl states produce control characters a keyboard has no business typing
  - dead keys: a composition becomes "dead key, then base key", and the
    standalone accent becomes "dead key, then space" via the KLC's own
    space entry — this is also how ASCII `~^ surface on layouts that only
    have them as dead keys
  - when a character is reachable several ways, the cheapest wins:
    fewer keystrokes, then fewer modifiers, then file order

What it refuses: ligatures and SGCaps rows (none of our layouts use them,
and silently mis-typing would be worse than failing loudly here).

Printable ASCII a layout genuinely cannot produce (Italian has no `` ` `` or
`~`) is listed in the generated header and on stderr: it will be SKIPPED at
runtime, a hole the reader can see rather than a wrong character.
"""

import argparse
import re
import sys

# Windows scancode (set 1) -> USB HID usage, for every key a .klc LAYOUT
# section can name on the main block. Numpad scancodes (0x53, 0x7e) are
# deliberately absent: transcripts are typed on the main block only.
SC_TO_HID = {
    0x02: 0x1E, 0x03: 0x1F, 0x04: 0x20, 0x05: 0x21, 0x06: 0x22,
    0x07: 0x23, 0x08: 0x24, 0x09: 0x25, 0x0A: 0x26, 0x0B: 0x27,
    0x0C: 0x2D, 0x0D: 0x2E,
    0x10: 0x14, 0x11: 0x1A, 0x12: 0x08, 0x13: 0x15, 0x14: 0x17,
    0x15: 0x1C, 0x16: 0x18, 0x17: 0x0C, 0x18: 0x12, 0x19: 0x13,
    0x1A: 0x2F, 0x1B: 0x30,
    0x1E: 0x04, 0x1F: 0x16, 0x20: 0x07, 0x21: 0x09, 0x22: 0x0A,
    0x23: 0x0B, 0x24: 0x0D, 0x25: 0x0E, 0x26: 0x0F,
    0x27: 0x33, 0x28: 0x34, 0x29: 0x35,
    0x2B: 0x32,  # ISO key next to Enter (Non-US Hash)
    0x2C: 0x1D, 0x2D: 0x1B, 0x2E: 0x06, 0x2F: 0x19, 0x30: 0x05,
    0x31: 0x11, 0x32: 0x10,
    0x33: 0x36, 0x34: 0x37, 0x35: 0x38,
    0x39: 0x2C,  # space
    0x56: 0x64,  # ISO key next to left Shift (Non-US Backslash)
    0x73: 0x87,  # ABNT C1
}

MOD_S = 0x02  # HID_MOD_LSHIFT
MOD_G = 0x40  # HID_MOD_RALT (AltGr)
STATE_TO_MOD = {0: 0, 1: MOD_S, 6: MOD_G, 7: MOD_S | MOD_G}
MOD_RANK = {0: 0, MOD_S: 1, MOD_G: 2, MOD_S | MOD_G: 3}
MOD_NAME = {0: "0", MOD_S: "S", MOD_G: "G", MOD_S | MOD_G: "S|G"}

HID_SPACE = 0x2C


def parse_cell(tok):
    """-> (codepoint or None, is_dead)."""
    dead = tok.endswith("@")
    if dead:
        tok = tok[:-1]
    if tok in ("-1", "%%"):
        return None, False
    if len(tok) == 1:
        return ord(tok), dead
    return int(tok, 16), dead


def parse_klc(path):
    raw = open(path, "rb").read()
    text = raw.decode("utf-16-le").lstrip("﻿")
    lines = [ln.rstrip() for ln in text.replace("\r\n", "\n").split("\n")]

    meta = {"header": []}
    for ln in lines[:6]:
        if ln.startswith(";"):
            meta["header"].append(ln.lstrip("; ").strip())

    section = None
    shift_states = []       # column order -> state number
    direct = {}             # cp -> list of (mod, usage, order)
    dead_pos = {}           # dead cp -> list of (mod, usage, order)
    dead_tables = {}        # dead cp -> {base cp: result cp}
    cur_dead = None
    order = 0

    keywords = {"SHIFTSTATE", "LAYOUT", "DEADKEY", "KEYNAME", "KEYNAME_EXT",
                "KEYNAME_DEAD", "DESCRIPTIONS", "LANGUAGENAMES", "ENDKBD",
                "ATTRIBUTES", "COPYRIGHT", "COMPANY", "LOCALENAME", "LOCALEID",
                "VERSION", "KBD", "LIGATURE", "MODIFIERS"}

    for ln in lines:
        toks = ln.split()
        cut = len(toks)
        for i, t in enumerate(toks):
            if t.startswith("//") or t.startswith(";"):
                cut = i
                break
        toks = toks[:cut]
        if not toks:
            continue

        if toks[0] in keywords:
            section = toks[0]
            if section == "DEADKEY":
                cur_dead = int(toks[1], 16)
                dead_tables[cur_dead] = {}
            if section == "LIGATURE":
                sys.exit("error: LIGATURE section — this generator cannot express it")
            if section == "KBD":
                meta["name"] = " ".join(toks[2:]).strip('"')
            if section == "LOCALENAME":
                meta["locale"] = toks[1].strip('"')
            continue

        if section == "SHIFTSTATE":
            shift_states.append(int(toks[0]))
        elif section == "LAYOUT":
            if len(toks) < 4:
                continue
            if toks[2].upper() == "SGCAP":
                sys.exit("error: SGCaps row — this generator cannot express it")
            sc = int(toks[0], 16)
            usage = SC_TO_HID.get(sc)
            cells = toks[3:]
            for col, tok in enumerate(cells):
                if col >= len(shift_states):
                    break
                mod = STATE_TO_MOD.get(shift_states[col])
                cp, dead = parse_cell(tok)
                if cp is None or mod is None:
                    continue
                if usage is None:
                    print(f"  note: scancode {sc:02x} not on the main block; "
                          f"dropped U+{cp:04X}", file=sys.stderr)
                    continue
                order += 1
                (dead_pos if dead else direct).setdefault(cp, []).append(
                    (mod, usage, order))
        elif section == "DEADKEY":
            if len(toks) >= 2:
                dead_tables[cur_dead][int(toks[0], 16)] = int(toks[1], 16)

    return meta, direct, dead_pos, dead_tables


def cheapest(cands):
    return min(cands, key=lambda c: (MOD_RANK[c[0]], c[2]))


def build_entries(direct, dead_pos, dead_tables):
    """-> {cp: (steps, note)} where steps is a 1- or 2-list of (mod, usage)."""
    out = {}
    for cp, cands in direct.items():
        if cp < 0x20 or cp == 0x7F or cp == 0xA0:  # controls; textnorm owns nbsp
            continue
        mod, usage, _ = cheapest(cands)
        out[cp] = ([(mod, usage)], "")

    for dcp, table in dead_tables.items():
        if dcp not in dead_pos:
            print(f"  note: dead key U+{dcp:04X} has a table but no key", file=sys.stderr)
            continue
        dmod, dusage, _ = cheapest(dead_pos[dcp])
        for base, result in table.items():
            if result in out:  # a direct key always beats a composition
                continue
            if result < 0x20 or result == 0x7F:
                continue
            if base not in direct:
                print(f"  note: U+{result:04X} needs base U+{base:04X}, "
                      f"which this layout cannot type; dropped", file=sys.stderr)
                continue
            bmod, busage, _ = cheapest(direct[base])
            out[result] = ([(dmod, dusage), (bmod, busage)],
                           f"dead {chr(dcp)} + {chr(base)}")
    return out


def c_escape(cp):
    ch = chr(cp)
    if ch == "'":
        return r"'\''"
    if ch == "\\":
        return r"'\\'"
    if 0x20 <= cp < 0x7F:
        return f"'{ch}'"
    return f"0x{cp:04X}"


def emit(meta, entries, name, symbol, klc_file):
    missing = [c for c in range(0x20, 0x7F) if c not in entries]
    if missing:
        print(f"  unreachable printable ASCII: "
              f"{' '.join(chr(c) for c in missing)}", file=sys.stderr)

    lines = []
    a = lines.append
    a(f"/* {meta.get('name', name)} ({meta.get('locale', '?')}).")
    a(" *")
    a(" * GENERATED by tools/gen_keymap.py — edit the generator, not this file.")
    for h in meta["header"]:
        a(f" * Source: {h}")
    a(f" * via the KLC export of {klc_file}, scancodes translated to HID usages.")
    a(" *")
    a(" * The AltGr column matches Windows and Linux, which both treat Right")
    a(" * Alt as AltGr for this layout. macOS builds its Option combinations")
    a(" * differently; on a macOS host those entries may come out wrong until")
    a(" * validated.")
    if missing:
        a(" *")
        a(" * This layout cannot type these printable ASCII characters at all;")
        a(" * they resolve as KEYMAP_SKIPPED (a visible hole, never a wrong")
        a(f" * character): {' '.join(chr(c) for c in missing)}")
    a(" */")
    a('#include "core/keymap.h"')
    a("")
    a("#define S HID_MOD_LSHIFT")
    a("#define G HID_MOD_RALT")
    a("")
    a("#define ONE(cp, m, k) {(cp), {{{(m), (k)}}, 1}}")
    a("#define TWO(cp, m1, k1, m2, k2) {(cp), {{{(m1), (k1)}, {(m2), (k2)}}, 2}}")
    a("")
    a(f"static const keymap_entry_t {symbol}_entries[] = {{")
    a("    ONE('\\t', 0, HID_KEY_TAB),")
    a("    ONE('\\n', 0, HID_KEY_ENTER),")

    def fmt(cp, steps, note):
        cmt = f" /* {chr(cp)}{': ' + note if note else ''} */" if cp > 0x7E or note else ""
        if len(steps) == 1:
            m, k = steps[0]
            return f"    ONE({c_escape(cp)}, {MOD_NAME[m]}, 0x{k:02X}),{cmt}"
        (m1, k1), (m2, k2) = steps
        return (f"    TWO({c_escape(cp)}, {MOD_NAME[m1]}, 0x{k1:02X}, "
                f"{MOD_NAME[m2]}, 0x{k2:02X}),{cmt}")

    for cp in sorted(entries):
        steps, note = entries[cp]
        a(fmt(cp, steps, note))
    a("};")
    a("")
    a(f"static bool {symbol}_lookup(uint32_t cp, hid_seq_t *out)")
    a("{")
    a(f"    return keymap_table_lookup({symbol}_entries,")
    a(f"                               sizeof({symbol}_entries) / sizeof({symbol}_entries[0]), cp, out);")
    a("}")
    a("")
    a(f"const keymap_layout_t {symbol} = {{")
    a(f'    .name   = "{name}",')
    a(f"    .lookup = {symbol}_lookup,")
    a("};")
    a("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("klc")
    ap.add_argument("--name", required=True, help='settings token, e.g. "fr"')
    ap.add_argument("--symbol", help="C symbol (default keymap_<name>)")
    ap.add_argument("--out", help="output path (default stdout)")
    args = ap.parse_args()

    symbol = args.symbol or "keymap_" + re.sub(r"[^a-z0-9]", "", args.name)
    meta, direct, dead_pos, dead_tables = parse_klc(args.klc)
    entries = build_entries(direct, dead_pos, dead_tables)
    import os
    code = emit(meta, entries, args.name, symbol, os.path.basename(args.klc))

    print(f"  {args.name}: {len(entries) + 2} entries", file=sys.stderr)
    if args.out:
        open(args.out, "w").write(code)
    else:
        sys.stdout.write(code)


if __name__ == "__main__":
    main()
