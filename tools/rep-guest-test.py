#!/usr/bin/env python3
"""REP MOVS / STOS under TCG with the fast path on and off (patch 17, M9
track): a DOS program runs a battery of `rep movs{b,w,d}` / `rep stos{b,w,d}`
cases — 16- and 32-bit address size, DF both ways, counts around the fast
path's threshold, runs crossing one and two pages, elements straddling a page
boundary, every overlap of source and destination (pattern fills included),
fill values with equal and distinct bytes — over a page-aligned 16 KiB region,
and prints a hash of the region plus the final ESI/EDI/ECX after each case on
COM1. Boots it on the FreeDOS test floppy (fetched by tools/x87-guest-test.py
on first use) under `-cpu pentium3,rep-fast=on` and `=off`, requires the two
logs to be identical, and checks every line against a Python model of the
instruction (so a bug shared by both paths is caught too).

    tools/rep-guest-test.py            # needs nasm, mtools, build/qemu

Outputs in build/rep-guest/.
"""
import importlib.util
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/rep-guest")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

PAGE = 4096
REGION = 4 * PAGE          # hashed, page-aligned in the guest's linear address space
MOVS, STOS = 0, 1
WIDTHS = {1: 0, 2: 1, 4: 2}


def cases():
    """(op, width, a32, df, src, dst, count, value): offsets relative to the region."""
    cs = []

    def C(op, w, a32, df, src, dst, count, value=0x12345678):
        cs.append((op, w, a32, df, src, dst, count, value))

    for op in (MOVS, STOS):
        for w in (1, 2, 4):
            for a32 in (0, 1):
                for df in (0, 1):
                    # the threshold and the page's end: 0..17 elements, forward from an
                    # offset 3 elements short of the page end, backward from 3 past it
                    for n in (0, 1, 7, 8, 9, 17):
                        if df:
                            C(op, w, a32, df, PAGE + 2 * w, 3 * PAGE + 2 * w, n)
                        else:
                            C(op, w, a32, df, PAGE - 3 * w, 3 * PAGE - 3 * w, n)
                    # aligned runs over one and two page boundaries
                    if df:
                        C(op, w, a32, df, 2 * PAGE - w, 4 * PAGE - w, 6000 // w)
                        C(op, w, a32, df, 3 * PAGE - w, 4 * PAGE - w, 9000 // w)
                    else:
                        C(op, w, a32, df, 0, 2 * PAGE + 512, 6000 // w)
                        C(op, w, a32, df, 100, PAGE + 100, 9000 // w)
                    # misaligned source and destination, elements straddling a page end
                    for ms in (1, 2, 3):
                        for md in (0, 3):
                            if df:
                                C(op, w, a32, df, 2 * PAGE - w - ms, 4 * PAGE - w - md, 5000 // w)
                            else:
                                C(op, w, a32, df, ms, 2 * PAGE + md, 5000 // w)
                    if w > 1:
                        for k in range(1, w):
                            if df:
                                C(op, w, a32, df, 2 * PAGE - k, 3 * PAGE - k - 1, 3000 // w)
                            else:
                                C(op, w, a32, df, PAGE - k, 3 * PAGE - k - 1, 3000 // w)
                    # fill values: equal bytes, distinct bytes, zero
                    if op == STOS:
                        for v in (0x11111111, 0xa5, 0):
                            if df:
                                C(op, w, a32, df, 0, 3 * PAGE - w, 5000 // w, v)
                            else:
                                C(op, w, a32, df, 0, PAGE + 40, 5000 // w, v)
                    # overlaps: every relation of source and destination in a run
                    # that crosses a page
                    if op == MOVS:
                        for d in (1, -1, w, -w, 100, -100, 0, 2 * w + 1, -(2 * w + 1), 4096, -4096):
                            if df:
                                C(op, w, a32, df, 3 * PAGE - 1 - w, 3 * PAGE - 1 - w + d, 7000 // w)
                            else:
                                C(op, w, a32, df, PAGE + 3 * w, PAGE + 3 * w + d, 7000 // w)
    return cs


def model(base, case):
    """The region's hash and ESI/EDI/ECX after the case, per the manual."""
    op, w, a32, df, src, dst, count, value = case
    mask = 0xffffffff if a32 else 0xffff
    n = count & mask
    step = -w if df else w
    esi = ((base + src + n * step) & mask) if op == MOVS else (base + src)
    edi = (base + dst + n * step) & mask
    ecx = (count - n) & mask
    return region_hash(case), esi, edi, ecx


def region_hash(case):
    """The hash the guest prints over the region after the case."""
    if case in HASHES:
        return HASHES[case]
    op, w, a32, df, src, dst, count, value = case
    mem = bytearray(((i * 13 + 7) & 0xff) for i in range(REGION))
    mask = 0xffffffff if a32 else 0xffff
    n = count & mask
    step = -w if df else w
    for k in range(n):
        d = dst + k * step
        assert 0 <= d and d + w <= REGION, case
        if op == MOVS:
            s = src + k * step
            assert 0 <= s and s + w <= REGION, case
            mem[d:d + w] = mem[s:s + w]
        else:
            mem[d:d + w] = (value & ((1 << (8 * w)) - 1)).to_bytes(w, "little")
    h = 0
    for b in mem:
        h = (((h << 5) | (h >> 27)) + b) & 0xffffffff
    HASHES[case] = h
    return h


HASHES = {}


ASM_HEAD = r"""
org 100h
bits 16
%define REGION 16384
%define CASE_SIZE 16

start:
    push cs
    pop ds
    push cs
    pop es
    ; base: the first page-aligned linear address in the area
    mov ax, cs
    movzx eax, ax
    shl eax, 4
    add eax, area
    and eax, 4095
    neg eax
    and eax, 4095
    add ax, area
    mov [base], ax
    mov si, str_base
    call puts
    mov ax, [base]
    call put_hex16
    mov al, 10
    call putc

    mov bx, cases
.next:
    cmp bx, cases_end
    jae .done
    call fill
    call run_case
    call hash_print
    add bx, CASE_SIZE
    jmp .next
.done:
    mov si, str_done
    call puts
    int 20h

fill:                           ; byte i = i * 13 + 7
    cld
    mov di, [base]
    mov cx, REGION
    mov al, 7
.l: stosb
    add al, 13
    loop .l
    ret

run_case:                       ; bx -> case: op, variant, df, -, src, dst, count, value
    cld
    cmp byte [bx + 2], 0
    je .fwd
    std
.fwd:
    xor esi, esi
    xor edi, edi
    xor ecx, ecx
    mov si, [base]
    add si, [bx + 4]
    mov di, [base]
    add di, [bx + 6]
    mov ecx, [bx + 8]
    mov eax, [bx + 12]
    movzx dx, byte [bx]
    imul dx, dx, 6
    add dl, [bx + 1]
    shl dx, 1
    add dx, table
    xchg dx, bx
    jmp [bx]
.mb16: rep movsb
    jmp .end
.mw16: rep movsw
    jmp .end
.md16: rep movsd
    jmp .end
.mb32: a32 rep movsb
    jmp .end
.mw32: a32 rep movsw
    jmp .end
.md32: a32 rep movsd
    jmp .end
.sb16: rep stosb
    jmp .end
.sw16: rep stosw
    jmp .end
.sd16: rep stosd
    jmp .end
.sb32: a32 rep stosb
    jmp .end
.sw32: a32 rep stosw
    jmp .end
.sd32: a32 rep stosd
.end:
    cld
    mov bx, dx
    mov [r_si], esi
    mov [r_di], edi
    mov [r_cx], ecx
    ret

table:
    dw run_case.mb16, run_case.mb32, run_case.mw16, run_case.mw32, run_case.md16, run_case.md32
    dw run_case.sb16, run_case.sb32, run_case.sw16, run_case.sw32, run_case.sd16, run_case.sd32

hash_print:                     ; h = rol(h, 5) + byte over the region
    mov si, [base]
    mov cx, REGION
    xor edx, edx
.l: rol edx, 5
    movzx eax, byte [si]
    add edx, eax
    inc si
    loop .l
    mov eax, edx
    call put_hex32
    mov al, ' '
    call putc
    mov eax, [r_si]
    call put_hex32
    mov al, ' '
    call putc
    mov eax, [r_di]
    call put_hex32
    mov al, ' '
    call putc
    mov eax, [r_cx]
    call put_hex32
    mov al, 10
    call putc
    ret

putc:
    push ax
    push dx
    mov dx, 3FDh
.w: in al, dx
    test al, 20h
    jz .w
    pop dx
    pop ax
    push dx
    mov dx, 3F8h
    out dx, al
    pop dx
    ret
puts:
    lodsb
    test al, al
    jz .d
    call putc
    jmp puts
.d: ret
put_hex32:
    push eax
    shr eax, 16
    call put_hex16
    pop eax
put_hex16:
    push ax
    mov al, ah
    call put_hex8
    pop ax
put_hex8:
    push ax
    shr al, 4
    call put_nib
    pop ax
put_nib:
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe .o
    add al, 'a' - '0' - 10
.o: jmp putc

str_base: db "BASE ", 0
str_done: db "DONE", 10, 0
base: dw 0
r_si: dd 0
r_di: dd 0
r_cx: dd 0

cases:
"""

ASM_TAIL = r"""
cases_end:
times (3000h - ($ - $$)) db 0
area:
"""


def build_asm(cs):
    lines = [ASM_HEAD]
    for op, w, a32, df, src, dst, count, value in cs:
        lines.append("    db %d, %d, %d, 0\n    dw %d, %d\n    dd %d, 0x%x\n"
                     % (op, WIDTHS[w] * 2 + a32, df, src, dst, count, value))
    lines.append(ASM_TAIL)
    return "".join(lines)


def run_qemu(fast, img, log):
    saved = x87gt.QEMU
    p = x87gt.subprocess.Popen([
        x87gt.QEMU, "-machine", "pc", "-cpu", "pentium3,rep-fast=" + fast, "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-fda", img, "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
    ])
    t0 = x87gt.time.time()
    try:
        while x87gt.time.time() - t0 < 600:
            x87gt.time.sleep(0.5)
            if p.poll() is not None:
                break
            if os.path.exists(log) and b"DONE" in open(log, "rb").read()[-16:]:
                break
        else:
            raise SystemExit("timeout waiting for DONE (rep-fast=%s)" % fast)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    assert saved == x87gt.QEMU
    return open(log, "rb").read().decode("ascii", "replace").split("\n")


def main():
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    cs = cases()
    for case in cs:
        region_hash(case)       # the model's bounds checks, before any boot
    asm = os.path.join(OUT, "reptest.asm")
    com = os.path.join(OUT, "REPTEST.COM")
    with open(asm, "w") as f:
        f.write(build_asm(cs))
    x87gt.sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    img = os.path.join(OUT, "reptest.img")
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nREPTEST.COM\r\n")
    x87gt.sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    x87gt.sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    x87gt.sh("mcopy", "-o", "-i", img, com, "::REPTEST.COM")

    logs = {}
    for fast in ("on", "off"):
        log = os.path.join(OUT, "serial-%s.log" % fast)
        if os.path.exists(log):
            os.unlink(log)
        lines = run_qemu(fast, img, log)
        start = next(i for i, l in enumerate(lines) if l.startswith("BASE "))
        base = int(lines[start].split()[1], 16)
        results = [l.strip() for l in lines[start + 1:] if len(l.split()) == 4]
        logs[fast] = (base, results)
        print("rep-fast=%s: base 0x%04x, %d result lines" % (fast, base, len(results)))

    bad = 0
    for fast in ("on", "off"):
        base, results = logs[fast]
        if len(results) != len(cs):
            print("FAIL: rep-fast=%s produced %d lines for %d cases" % (fast, len(results), len(cs)))
            bad += 1
            continue
        for i, (case, line) in enumerate(zip(cs, results)):
            got = tuple(int(x, 16) for x in line.split())
            exp = model(base, case)
            if got != exp:
                bad += 1
                if bad <= 20:
                    print("FAIL: rep-fast=%s case %d %s: got %s expected %s"
                          % (fast, i, case, " ".join("%08x" % x for x in got),
                             " ".join("%08x" % x for x in exp)))
    if logs["on"][1] != logs["off"][1]:
        print("FAIL: rep-fast=on and =off logs differ")
        bad += 1
    if bad:
        print("FAIL: %d mismatches over %d cases" % (bad, len(cs)))
        return 1
    print("PASS: %d cases, rep-fast=on == off == model" % len(cs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
