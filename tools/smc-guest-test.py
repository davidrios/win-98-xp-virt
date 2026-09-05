#!/usr/bin/env python3
"""Self-modifying code under TCG with the same-value store skip on and off
(patch 18, M9 track): a DOS program patches its own instructions the ways the
software-rendered games do — an immediate rewritten with new values from
another block and from the block being executed (precise SMC), rewritten
with the value already there, an opcode byte flipped, 16- and 8-bit partial
patches of an imm32, a routine overwritten by `rep movsd` (the probe path)
with new and with identical bytes, and an imm32 straddling a page boundary
written by one crossing store — and prints a checksum of what the patched
code computed. Boots it on the FreeDOS test floppy (fetched by
tools/x87-guest-test.py on first use) under `-accel tcg,smc-same-value=on`
and `=off`; every checksum must equal the architectural result.

    tools/smc-guest-test.py            # needs nasm, mtools, build/qemu

Outputs in build/smc-guest/.
"""
import importlib.util
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/smc-guest")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

N = 1000
M = 0xffffffff
EXPECTED = {
    "A": sum(range(N)) & M,                                    # imm32 patched from another block
    "B": (N * 0x12345678) & M,                                 # the same imm32 rewritten each time
    "C": sum(range(N)) & M,                                    # patched inside the executing block
    "D": (N * 0x77) & M,                                       # same value inside the executing block
    "E": sum(1000 + i if i % 2 == 0 else 1000 - i for i in range(N)) & M,  # opcode add <-> sub
    "F": sum((i & 0xffff) | ((i & 0xff) << 24) for i in range(N)) & M,      # 16-bit + 8-bit partial patches
    "G": (0x2222 + 0x2222 + 0x1111) & M,                       # rep movsd: new body, same body, old body
    "H": sum(range(N)) & M,                                    # imm32 across a page boundary, new values
    "I": (N * 0x5555) & M,                                     # the same, rewritten with the same value
}

ASM = r"""
org 100h
bits 16
%define N 1000

start:
    push cs
    pop ds
    push cs
    pop es
    ; hbase: a routine placed so that its imm32 (at +2) straddles a page boundary
    mov ax, cs
    movzx eax, ax
    shl eax, 4
    add eax, area + 16
    and eax, 4095
    neg eax
    and eax, 4095
    add ax, area + 16
    sub ax, 4
    mov [hbase], ax

    ; A: patch the imm32 of routA from another block, values 0..N-1
    xor ebx, ebx
    xor edi, edi
.la:
    mov [routA + 2], ebx
    call routA
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .la
    mov al, 'A'
    call report

    ; B: the same value written before every call
    mov dword [routA + 2], 12345678h
    xor ebx, ebx
    xor edi, edi
.lb:
    mov dword [routA + 2], 12345678h
    call routA
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .lb
    mov al, 'B'
    call report

    ; C: the next instruction of the executing block patched with new values
    xor ebx, ebx
    xor edi, edi
.lc:
    mov [.cins + 2], ebx
.cins:
    mov eax, 0
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .lc
    mov al, 'C'
    call report

    ; D: the same, with the value already there
    mov dword [.dins + 2], 77h
    xor ebx, ebx
    xor edi, edi
.ld:
    mov dword [.dins + 2], 77h
.dins:
    mov eax, 0
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .ld
    mov al, 'D'
    call report

    ; E: the opcode of routE alternates between add eax,ebx (01 D8) and sub (29 D8)
    xor ecx, ecx
    xor edi, edi
.le:
    mov al, 01h
    test ecx, 1
    jz .e1
    mov al, 29h
.e1:
    mov [routE + 1], al
    mov eax, 1000
    mov ebx, ecx
    call routE
    add edi, eax
    inc ecx
    cmp ecx, N
    jb .le
    mov al, 'E'
    call report

    ; F: a 16-bit store into the low half of routA's imm32 and a byte into its top
    mov dword [routA + 2], 0
    xor ecx, ecx
    xor edi, edi
.lf:
    mov [routA + 2], cx
    mov [routA + 5], cl
    call routA
    add edi, eax
    inc ecx
    cmp ecx, N
    jb .lf
    mov al, 'F'
    call report

    ; G: routG overwritten by rep movsd: a new body, the same body again, the old one
    xor ebp, ebp
    cld
    mov si, tmplG2
    mov di, routG
    mov cx, 2
    rep movsd
    call routG
    add ebp, eax
    mov si, tmplG2
    mov di, routG
    mov cx, 2
    rep movsd
    call routG
    add ebp, eax
    mov si, tmplG1
    mov di, routG
    mov cx, 2
    rep movsd
    call routG
    add ebp, eax
    mov edi, ebp
    mov al, 'G'
    call report

    ; H: the routine at hbase (mov eax, imm32; ret) has its imm32 across a page
    ; boundary; one 4-byte store patches it with new values
    mov si, tmplH
    mov di, [hbase]
    mov cx, 7
    rep movsb
    xor ebx, ebx
    xor edi, edi
.lh:
    mov si, [hbase]
    add si, 2
    mov [si], ebx
    call word [hbase]
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .lh
    mov al, 'H'
    call report

    ; I: the same crossing store with the value already there
    mov si, [hbase]
    add si, 2
    mov dword [si], 5555h
    xor ebx, ebx
    xor edi, edi
.li:
    mov si, [hbase]
    add si, 2
    mov dword [si], 5555h
    call word [hbase]
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .li
    mov al, 'I'
    call report

    mov si, str_done
    call puts
    int 20h

report:                         ; al = case letter, edi = checksum
    call putc
    mov al, ' '
    call putc
    mov eax, edi
    call put_hex32
    mov al, 10
    call putc
    ret

routA:
    mov eax, 0
    ret
routE:
    add eax, ebx
    ret
align 4
routG:                          ; 66 B8 imm32 C3 90: 8 bytes
    mov eax, 1111h
    ret
    nop
tmplG1:
    mov eax, 1111h
    ret
    nop
tmplG2:
    mov eax, 2222h
    ret
    nop
tmplH:                          ; mov eax, imm32; ret — 7 bytes, the imm32 at +2
    db 66h, 0B8h
    dd 0
    db 0C3h

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

str_done: db "DONE", 10, 0
hbase: dw 0
times (1000h - ($ - $$)) db 0
area: times 8192 db 0
"""


def run_qemu(mode, img, log):
    p = x87gt.subprocess.Popen([
        x87gt.QEMU, "-machine", "pc", "-accel", "tcg,smc-same-value=" + mode,
        "-cpu", "pentium3", "-m", "64",
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
            raise SystemExit("timeout waiting for DONE (smc-same-value=%s)" % mode)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    return open(log, "rb").read().decode("ascii", "replace").split("\n")


def main():
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    asm = os.path.join(OUT, "smctest.asm")
    com = os.path.join(OUT, "SMCTEST.COM")
    with open(asm, "w") as f:
        f.write(ASM)
    x87gt.sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    img = os.path.join(OUT, "smctest.img")
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nSMCTEST.COM\r\n")
    x87gt.sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    x87gt.sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    x87gt.sh("mcopy", "-o", "-i", img, com, "::SMCTEST.COM")

    bad = 0
    for mode in ("on", "off"):
        log = os.path.join(OUT, "serial-%s.log" % mode)
        if os.path.exists(log):
            os.unlink(log)
        lines = run_qemu(mode, img, log)
        got = {}
        for l in lines:
            parts = l.split()
            if len(parts) == 2 and parts[0] in EXPECTED and len(parts[1]) == 8:
                got[parts[0]] = int(parts[1], 16)
        for k in sorted(EXPECTED):
            if k not in got:
                print("FAIL: smc-same-value=%s case %s missing" % (mode, k))
                bad += 1
            elif got[k] != EXPECTED[k]:
                print("FAIL: smc-same-value=%s case %s: got %08x expected %08x" % (mode, k, got[k], EXPECTED[k]))
                bad += 1
        print("smc-same-value=%s: %d/%d cases right" % (mode, len(EXPECTED) - sum(1 for k in EXPECTED if got.get(k) != EXPECTED[k]), len(EXPECTED)))
    if bad:
        print("FAIL: %d mismatches" % bad)
        return 1
    print("PASS: %d cases, smc-same-value=on == off == the architecture" % len(EXPECTED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
