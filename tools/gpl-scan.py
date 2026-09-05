"""QEMU source files whose header pins them to GPL version 2 *only*.

    tools/gpl-scan.py qemu target/i386 accel/tcg system hw/i386 hw/ide \
        hw/display hw/audio hw/net hw/pci hw/char hw/usb hw/block \
        audio net block ui io util qom migration

Run it after a QEMU bump: ADR-010 turns on whether *any* file compiled into
`libqemu-embed-<target>` is v2-only (2026-09-05: 35 of them, `util/bitmap.c`
and `hw/audio/ac97.c` among them), because a single one pins the player to
GPL-2.0-only and rules out the `-or-later` move ADR-009 made for the
launcher. The scan is deliberately crude and over-reports rather than
under-reports; check anything new by hand.

QEMU's LICENSE says a file with no licensing information is GPLv2-or-later,
and that v2-only contributions are accepted only for bsd-user/, linux-user/,
hw/vfio/ and hw/xen/xen_pt*. This checks the claim against the files an
i386 softmmu build would actually compile: read each header, and flag one
that says "version 2" without any "later" nearby.
"""
import os
import re
import sys

roots = sys.argv[2:]
base = sys.argv[1]
only, orlater, quiet = [], 0, 0
for root in roots:
    for dirpath, _dirs, files in os.walk(os.path.join(base, root)):
        for name in files:
            if not name.endswith((".c", ".h", ".c.inc", ".h.inc")):
                continue
            path = os.path.join(dirpath, name)
            try:
                head = open(path, encoding="utf-8", errors="replace").read(4000)
            except OSError:
                continue
            # the SPDX tag wins where there is one
            spdx = re.search(r"SPDX-License-Identifier:\s*([^\n*]+)", head)
            if spdx:
                tag = spdx.group(1).strip()
                if "GPL-2.0-only" in tag:
                    only.append((os.path.relpath(path, base), tag))
                elif "GPL-2.0-or-later" in tag:
                    orlater += 1
                else:
                    quiet += 1
                continue
            m = re.search(r"(GNU General Public License|GNU GPL|General Public License)", head)
            if not m:
                quiet += 1
                continue
            window = head[m.start():m.start() + 400].lower()
            if "version 2" in window or "gplv2" in window or "gpl, v2" in window:
                if "later" in window or "version 2 or" in window:
                    orlater += 1
                else:
                    only.append((os.path.relpath(path, base), window.split("\n")[0][:90]))
            else:
                quiet += 1

print("v2-or-later headers: %d    no GPL header (=> v2-or-later per LICENSE): %d" % (orlater, quiet))
print("v2-ONLY headers: %d" % len(only))
for path, why in sorted(only):
    print("   %-52s %s" % (path, why))
