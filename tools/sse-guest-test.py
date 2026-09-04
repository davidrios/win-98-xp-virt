#!/usr/bin/env python3
"""In-guest regression test for the SSE inline fast path (patch 07, doc 16).

Builds a DOS program (NASM, .COM) that enables SSE in real mode and runs a
battery of SSE/SSE2 instruction sequences over every pair of a pool of
edge-case operands (packed lanes carry neighbouring pool entries) under
three MXCSR settings, streaming every result (all four lanes + MXCSR) to
the serial port. Boots it on the FreeDOS test floppy under our
qemu-system-i386 twice, `-cpu pentium3,+sse2,sse-fast=on` and `=off`, and
requires the two serial logs to be identical: the inline path must be
indistinguishable from the helpers (softfloat), flags included.

    tools/sse-guest-test.py            # needs nasm, mtools, build/qemu; fetches the FreeDOS floppy

The MXCSR settings: 1FA0 (default, PE already sticky: the inline mode),
1F80 (flags clear: the first inexact helper must hand over to the inline
mode mid-block), 3FA0 (round down: mode off, helpers only). A bench
program (SSEBENCH.COM) times a packed and a scalar kernel with the BIOS
tick counter under both settings and the script reports the ratio.
"""
import os
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib
x87 = importlib.import_module("x87-guest-test")

ROOT = x87.ROOT
QEMU = x87.QEMU
FLOPPY = x87.FLOPPY
OUT = os.path.join(ROOT, "build/sse-guest")


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f64(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


# ---------------------------------------------------------------- pools

def single_values():
    v = [0x00000000, 0x80000000]                                   # +-0
    v += [f32(x) for x in (1.0, -1.0, 0.5, 1.5, 3.0, 1234567.0, -42.25, 1e-5,
                           0.1, 1.0 / 3.0, 0.9999999, 1.0000001, 2.5, -2.5,
                           3.5, 1e10, 3e20, 7e-20, -6.5e-3, 65536.0,
                           16777216.0, 0.4999999, 3.4e38, 1.17e-38)]
    v += [0x00800000, 0x01000000, 0x7f7fffff, 0x7f000000]          # FLT_MIN, 2^-125, FLT_MAX, 2^127
    v += [0x00400000, 0x00000001, 0x007fffff, 0x80000001]          # denormals
    v += [0x7f800000, 0xff800000]                                  # +-inf
    v += [0x7fc00000, 0xffc00000, 0x7f800001, 0x7fa00000]          # qnan, -qnan, snan, snan
    v += [0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001]          # around 2^31
    v += [0x5f000000, 0x20000000]                                  # 2^63, 2^-63
    v += [0x00000100]                                              # 1e-40-ish denormal
    return v


def double_values():
    v = [0, 1 << 63]
    v += [f64(x) for x in (1.0, -1.0, 0.5, 1.5, 3.0, 1234567.0, -42.25, 1e-5,
                           0.1, 1.0 / 3.0, 2.5, -2.5, 1e10, 1e300, 1e-300,
                           2147483647.5, 2147483647.4, -2147483648.5,
                           -2147483648.6, 2147483648.0, 16777217.0,
                           3.4028234663852886e38, 1e-45)]
    v += [0x0010000000000000, 0x7fefffffffffffff]                  # DBL_MIN, DBL_MAX
    v += [0x0000000000000001, 0x000fffffffffffff]                  # denormals
    v += [0x7ff0000000000000, 0xfff0000000000000]                  # +-inf
    v += [0x7ff8000000000000, 0x7ff0000000000001]                  # qnan, snan
    return v


def pool_bytes(values, lanes, width):
    """entry i = [v[i], v[i+1], ...] (mod n), 16 bytes each"""
    n = len(values)
    fmt = "<I" if width == 4 else "<Q"
    lines = []
    for i in range(n):
        b = b"".join(struct.pack(fmt, values[(i + k) % n]) for k in range(lanes))
        lines.append("    db " + ", ".join("0%02xh" % x for x in b))
    return "\n".join(lines)


# ---------------------------------------------------------------- ops
# Each op: xmm0 = a (entry i), xmm1 = b (entry j), bx -> a in memory,
# si -> b in memory; result in xmm0, MXCSR is read afterwards.

def eflags(insn):
    return insn + "\n    pushfd\n    pop eax\n    and eax, 08d5h\n    movd xmm0, eax"


SINGLE_OPS = [
    ("addps", "addps xmm0, xmm1"),
    ("addss", "addss xmm0, xmm1"),
    ("subps", "subps xmm0, xmm1"),
    ("subss_m", "subss xmm0, [si]"),
    ("mulps", "mulps xmm0, xmm1"),
    ("mulss", "mulss xmm0, xmm1"),
    ("divps", "divps xmm0, xmm1"),
    ("divss_m", "divss xmm0, [si]"),
    ("minps", "minps xmm0, xmm1"),
    ("minss", "minss xmm0, xmm1"),
    ("maxps_m", "maxps xmm0, [si]"),
    ("maxss", "maxss xmm0, xmm1"),
    ("sqrtps", "sqrtps xmm0, xmm1"),
    ("sqrtss", "sqrtss xmm0, xmm1"),
    ("rcpps", "rcpps xmm0, xmm1"),
    ("rcpss", "rcpss xmm0, xmm1"),
    ("rsqrtps", "rsqrtps xmm0, xmm1"),
    ("rsqrtss", "rsqrtss xmm0, xmm1"),
] + [("cmpps%d" % p, "cmpps xmm0, xmm1, %d" % p) for p in range(8)] + [
    ("cmpss1", "cmpss xmm0, xmm1, 1"),
    ("cmpss4_m", "cmpss xmm0, [si], 4"),
    ("comiss", eflags("comiss xmm0, xmm1")),
    ("ucomiss", eflags("ucomiss xmm0, xmm1")),
    ("comiss_m", eflags("comiss xmm0, [si]")),
    ("cvttss2si", "cvttss2si eax, xmm1\n    movd xmm0, eax"),
    ("cvtss2si", "cvtss2si eax, xmm1\n    movd xmm0, eax"),
    ("cvtss2si_m", "cvtss2si eax, [si]\n    movd xmm0, eax"),
    ("cvtsi2ss", "movd eax, xmm1\n    cvtsi2ss xmm0, eax"),
    ("cvtsi2ss_m", "cvtsi2ss xmm0, dword [si]"),
    ("chain_p", "mulps xmm0, xmm1\n    addps xmm0, xmm1\n    subps xmm0, xmm1\n    divps xmm0, xmm1"),
    ("chain_s", "mulss xmm0, xmm1\n    addss xmm0, xmm1\n    divss xmm0, xmm1\n    sqrtss xmm0, xmm0\n"
                "    cvttss2si eax, xmm0\n    cvtsi2ss xmm1, eax\n    minss xmm0, xmm1"),
    ("mixed_x87", "fld dword [bx]\n    mulps xmm0, xmm1\n    fmul dword [si]\n    fadd dword [si + 4]\n"
                  "    addss xmm0, xmm1\n    fstp dword [tmp]\n    addss xmm0, [tmp]\n"
                  "    fld dword [bx + 4]\n    subps xmm0, xmm1\n    fstp dword [tmp]"),
    ("long", "\n".join(["    addps xmm0, xmm1"] * 60 + ["    subps xmm0, xmm1"] * 60).lstrip()),
]

def mmx(body):
    return ("movq mm0, [bx]\n    movq mm1, [si]\n    " + body +
            "\n    movq [res], mm0\n    movups xmm0, [res]\n    emms")


# integer / permutation ops (patch 08, simd-fast): MMX forms on the low 8
# bytes of the entries, XMM forms on all 16; pure bit patterns, no modes
INT_OPS = [
    ("shufps_1b", "shufps xmm0, xmm1, 1bh"),
    ("shufps_4e", "shufps xmm0, xmm1, 4eh"),
    ("shufps_b1_m", "shufps xmm0, [si], 0b1h"),
    ("shufps_00", "shufps xmm0, xmm1, 0"),
    ("unpcklps", "unpcklps xmm0, xmm1"),
    ("unpckhps_m", "unpckhps xmm0, [si]"),
] + [("mmx_" + n, mmx(b)) for n, b in [
    ("punpcklbw", "punpcklbw mm0, mm1"), ("punpcklwd", "punpcklwd mm0, mm1"),
    ("punpckldq_m", "punpckldq mm0, [si]"), ("punpckhbw", "punpckhbw mm0, mm1"),
    ("punpckhwd", "punpckhwd mm0, mm1"), ("punpckhdq", "punpckhdq mm0, mm1"),
    ("packsswb", "packsswb mm0, mm1"), ("packuswb", "packuswb mm0, mm1"),
    ("packssdw_m", "packssdw mm0, [si]"), ("pmulhw", "pmulhw mm0, mm1"),
    ("pmulhuw", "pmulhuw mm0, mm1"), ("pmaddwd", "pmaddwd mm0, mm1"),
    ("pavgb", "pavgb mm0, mm1"), ("pavgw_m", "pavgw mm0, [si]"),
    ("psadbw", "psadbw mm0, mm1"), ("psllw", "psllw mm0, mm1"),
    ("pslld", "pslld mm0, mm1"), ("psllq", "psllq mm0, mm1"),
    ("psrlw", "psrlw mm0, mm1"), ("psrld_m", "psrld mm0, [si]"),
    ("psrlq", "psrlq mm0, mm1"), ("psraw", "psraw mm0, mm1"),
    ("psrad", "psrad mm0, mm1"), ("pshufw_1b", "pshufw mm0, mm1, 1bh"),
    ("pshufw_b1_m", "pshufw mm0, [si], 0b1h"),
    ("self_unpck", "punpcklbw mm0, mm0\n    punpckhwd mm0, mm0"),
    ("shift_self", "psllw mm0, mm0"),
    ("chain", "punpcklbw mm0, mm1\n    pmulhw mm0, mm1\n    paddw mm0, mm1\n    psraw mm0, mm1\n    packuswb mm0, mm1\n    pavgb mm0, mm1"),
]] + [
    ("x87_mmx", "fld dword [bx]\n    fmul dword [si]\n    " + mmx("paddw mm0, mm1\n    pmulhw mm0, mm1") + "\n    fstp dword [tmp]"),
]

INT_OPS_SSE2 = [
    ("shufpd_1", "shufpd xmm0, xmm1, 1"), ("shufpd_2_m", "shufpd xmm0, [si], 2"),
    ("unpcklpd", "unpcklpd xmm0, xmm1"), ("unpckhpd", "unpckhpd xmm0, xmm1"),
    ("punpcklqdq", "punpcklqdq xmm0, xmm1"), ("punpckhqdq_m", "punpckhqdq xmm0, [si]"),
    ("xpunpcklbw", "punpcklbw xmm0, xmm1"), ("xpunpcklwd", "punpcklwd xmm0, xmm1"),
    ("xpunpckldq", "punpckldq xmm0, xmm1"), ("xpunpckhbw_m", "punpckhbw xmm0, [si]"),
    ("xpunpckhwd", "punpckhwd xmm0, xmm1"), ("xpunpckhdq", "punpckhdq xmm0, xmm1"),
    ("xpacksswb", "packsswb xmm0, xmm1"), ("xpackuswb_m", "packuswb xmm0, [si]"),
    ("xpackssdw", "packssdw xmm0, xmm1"), ("xpmulhw", "pmulhw xmm0, xmm1"),
    ("xpmulhuw", "pmulhuw xmm0, xmm1"), ("xpmaddwd", "pmaddwd xmm0, xmm1"),
    ("xpavgb", "pavgb xmm0, xmm1"), ("xpavgw", "pavgw xmm0, xmm1"),
    ("xpsadbw", "psadbw xmm0, xmm1"), ("xpsllw", "psllw xmm0, xmm1"),
    ("xpslld", "pslld xmm0, xmm1"), ("xpsllq_m", "psllq xmm0, [si]"),
    ("xpsrlw", "psrlw xmm0, xmm1"), ("xpsrld", "psrld xmm0, xmm1"),
    ("xpsrlq", "psrlq xmm0, xmm1"), ("xpsraw", "psraw xmm0, xmm1"),
    ("xpsrad", "psrad xmm0, xmm1"),
    ("xchain", "punpcklbw xmm0, xmm1\n    pmulhw xmm0, xmm1\n    psraw xmm0, xmm1\n    packuswb xmm0, xmm1\n    shufps xmm0, xmm1, 4eh"),
]

DOUBLE_OPS = [
    ("addpd", "addpd xmm0, xmm1"),
    ("addsd", "addsd xmm0, xmm1"),
    ("subsd_m", "subsd xmm0, [si]"),
    ("mulpd", "mulpd xmm0, xmm1"),
    ("mulsd", "mulsd xmm0, xmm1"),
    ("divpd", "divpd xmm0, xmm1"),
    ("divsd", "divsd xmm0, xmm1"),
    ("minpd", "minpd xmm0, xmm1"),
    ("maxsd", "maxsd xmm0, xmm1"),
    ("sqrtpd", "sqrtpd xmm0, xmm1"),
    ("sqrtsd", "sqrtsd xmm0, xmm1"),
    ("cmppd1", "cmppd xmm0, xmm1, 1"),
    ("cmpsd6", "cmpsd xmm0, xmm1, 6"),
    ("comisd", eflags("comisd xmm0, xmm1")),
    ("ucomisd", eflags("ucomisd xmm0, xmm1")),
    ("cvttsd2si", "cvttsd2si eax, xmm1\n    movd xmm0, eax"),
    ("cvtsd2si", "cvtsd2si eax, xmm1\n    movd xmm0, eax"),
    ("cvtsi2sd", "movd eax, xmm1\n    cvtsi2sd xmm0, eax"),
    ("cvtsd2ss", "cvtsd2ss xmm0, xmm1"),
    ("cvtss2sd", "cvtss2sd xmm0, xmm1"),
    ("chain_d", "mulsd xmm0, xmm1\n    addpd xmm0, xmm1\n    cvtsd2ss xmm0, xmm0\n    cvtss2sd xmm0, xmm0\n"
                "    divsd xmm0, xmm1"),
]

ASM = r"""
org 100h
bits 16

%define NS {ns}
%define ND {nd}
%define NUM_S_OPS {num_s_ops}
%define NUM_OPS {num_ops}

start:
    mov eax, cr0
    and eax, ~4                 ; CR0.EM = 0
    or eax, 2                   ; CR0.MP
    mov cr0, eax
    mov eax, cr4
    or eax, 600h                ; OSFXSR | OSXMMEXCPT
    mov cr4, eax
    fninit
    fldcw [cw]
    mov eax, 1
    cpuid
    test edx, 1 << 26
    setnz [have_sse2]
    mov si, str_sse2
    call puts
    mov al, [have_sse2]
    call put_hex8
    call newline

    mov si, mx_table
.mx_loop:
    mov eax, [si]
    cmp eax, -1
    je .done
    mov [cur_mx], eax
    add si, 4
    mov [mx_ptr], si
    mov si, str_mx
    call puts
    mov eax, [cur_mx]
    call put_hex32
    call newline
    xor cx, cx
.op_loop:
    cmp cx, NUM_OPS
    jae .ops_done
    mov [cur_op_idx], cx
    mov bx, cx
    shl bx, 1
    mov ax, [op_table + bx]
    mov [cur_op], ax
    mov word [pool_a], pool_s
    mov word [pool_b], pool_s
    mov word [pool_n], NS
    cmp cx, NUM_S_OPS
    jb .have_pool
    cmp byte [have_sse2], 0
    je .next_op
    mov word [pool_a], pool_d
    mov word [pool_b], pool_d
    mov word [pool_n], ND
.have_pool:
    mov si, str_op
    call puts
    mov al, cl
    call put_hex8
    call newline
    xor bp, bp
.i_loop:
    xor di, di
.j_loop:
    mov bx, bp
    shl bx, 4
    add bx, [pool_a]
    mov si, di
    shl si, 4
    add si, [pool_b]
    ldmxcsr [cur_mx]
    movups xmm0, [bx]
    movups xmm1, [si]
    call [cur_op]
    stmxcsr [mx_out]
    movups [res], xmm0
    mov ax, bp
    call put_hex8
    mov al, ' '
    call putc
    mov ax, di
    call put_hex8
    mov al, ' '
    call putc
    mov eax, [res]
    call put_hex32
    mov eax, [res + 4]
    call put_hex32
    mov eax, [res + 8]
    call put_hex32
    mov eax, [res + 12]
    call put_hex32
    mov al, ' '
    call putc
    mov eax, [mx_out]
    call put_hex32
    call newline
    inc di
    cmp di, [pool_n]
    jb .j_loop
    inc bp
    cmp bp, [pool_n]
    jb .i_loop
.next_op:
    mov cx, [cur_op_idx]
    inc cx
    jmp .op_loop
.ops_done:
    mov si, [mx_ptr]
    jmp .mx_loop
.done:
    mov si, str_done
    call puts
    int 20h

{ops}

putc:                       ; al = char -> COM1 (0x3F8), poll THR empty
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

puts:                       ; si = asciiz
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

newline:
    mov al, 10
    jmp putc

str_sse2: db "SSE2 ", 0
str_mx:   db "MX ", 0
str_op:   db "OP ", 0
str_done: db "DONE", 10, 0

cw: dw 027Fh                ; PC=53, RC nearest, all masked (x87 inline mode)
mx_table: dd 1FA0h, 1F80h, 3FA0h, -1
have_sse2: db 0
cur_mx: dd 0
mx_out: dd 0
mx_ptr: dw 0
cur_op: dw 0
cur_op_idx: dw 0
pool_a: dw 0
pool_b: dw 0
pool_n: dw 0
tmp: dd 0

op_table:
{op_table}

align 16
res: times 16 db 0
align 16
pool_s:
{pool_s}
align 16
pool_d:
{pool_d}
"""

BENCH_ASM = r"""
org 100h
bits 16
; SSE throughput: ITER iterations of a packed kernel (SSEBENCH: 8 packed
; ops on 4-wide vectors, the shape of a D3DX transform) and of a scalar
; kernel (SSEBENCHS: mulss/addss/divss/sqrtss/comiss/cvttss2si), timed with
; the BIOS tick counter (18.2 Hz). Prints "SSEBENCH <iterations> <ticks>"
; and "SSEBENCHS ..." on COM1.
%define ITER 40000000
start:
    mov eax, cr0
    and eax, ~4
    or eax, 2
    mov cr0, eax
    mov eax, cr4
    or eax, 600h
    mov cr4, eax
    ldmxcsr [mx]
    movaps xmm1, [va]
    movaps xmm2, [vb]
    movaps xmm3, [vc]
    movaps xmm4, [vd]
    movaps xmm5, [ve]
    movaps xmm7, [one]
    xor ax, ax
    mov es, ax
    mov si, [es:046Ch]
.sync:                          ; align to a tick edge
    mov ax, [es:046Ch]
    cmp ax, si
    je .sync
    mov [t0], ax
    mov ecx, ITER
.loop:                          ; register operands only: the SSE ops are the cost
    movaps xmm0, xmm1
    mulps xmm0, xmm2
    addps xmm0, xmm3
    movaps xmm6, xmm0
    mulps xmm6, xmm6
    addps xmm0, xmm6
    divps xmm0, xmm4
    maxps xmm0, xmm3
    minps xmm0, xmm5
    subps xmm0, xmm7
    dec ecx
    jnz .loop
    mov ax, [es:046Ch]
    sub ax, [t0]
    push ax
    mov si, str_bench
    call report

    mov si, [es:046Ch]
.sync2:
    mov ax, [es:046Ch]
    cmp ax, si
    je .sync2
    mov [t0], ax
    mov ecx, ITER
    xor ebx, ebx
.loop2:
    movss xmm0, xmm1
    mulss xmm0, xmm2
    addss xmm0, xmm3
    divss xmm0, xmm4
    sqrtss xmm6, xmm0
    addss xmm0, xmm6
    comiss xmm0, xmm7
    jb .skip
    subss xmm0, xmm7
.skip:
    cvttss2si eax, xmm0
    add ebx, eax
    dec ecx
    jnz .loop2
    mov ax, [es:046Ch]
    sub ax, [t0]
    push ax
    mov si, str_benchs
    call report

    movq mm1, [va]
    movq mm2, [vb]
    movq mm3, [vc]
    movq mm4, [shcnt]
    mov si, [es:046Ch]
.sync3:
    mov ax, [es:046Ch]
    cmp ax, si
    je .sync3
    mov [t0], ax
    mov ecx, ITER
.loop3:                         ; MMX blit-style chain, registers only (8 ops)
    movq mm0, mm1
    punpcklbw mm0, mm2
    pmulhw mm0, mm3
    psraw mm0, mm4
    paddw mm0, mm2
    packuswb mm0, mm3
    pavgb mm0, mm2
    psadbw mm0, mm1
    pshufw mm0, mm0, 1bh
    dec ecx
    jnz .loop3
    emms
    mov ax, [es:046Ch]
    sub ax, [t0]
    push ax
    mov si, str_benchm
    call report
    int 20h

report:                     ; si = tag, [sp+2] = ticks
    call puts
    mov eax, ITER
    call put_hex32
    mov al, ' '
    call putc
    pop bx
    pop ax
    push bx
    call put_hex16
    mov al, 10
    jmp putc

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

str_bench:  db "SSEBENCH ", 0
str_benchs: db "SSEBENCHS ", 0
str_benchm: db "SSEBENCHM ", 0
shcnt: dd 3, 0
mx: dd 1FA0h
t0: dw 0
align 16
va: dd 1.5, 2.5, -0.75, 3.25
vb: dd 0.9999, 1.0001, 2.0, 0.5
vc: dd 0.125, -0.25, 0.5, 1.0
vd: dd 3.0, 7.0, 1.25, 9.5
ve: dd 100.0, 100.0, 100.0, 100.0
one: dd 1.0, 1.0, 1.0, 1.0
vr: dd 0, 0, 0, 0
"""


def build_ops():
    ops = SINGLE_OPS + INT_OPS + DOUBLE_OPS + INT_OPS_SSE2
    text = []
    table = []
    for k, (name, body) in enumerate(ops):
        text.append("op_%d:  ; %s\n    %s\n    ret" % (k, name, body))
        table.append("    dw op_%d" % k)
    return "\n\n".join(text), "\n".join(table), len(SINGLE_OPS), len(ops)


def build_asm():
    sv, dv = single_values(), double_values()
    ops, table, num_s, num = build_ops()
    return ASM.format(ns=len(sv), nd=len(dv), num_s_ops=num_s, num_ops=num,
                      ops=ops, op_table=table,
                      pool_s=pool_bytes(sv, 4, 4), pool_d=pool_bytes(dv, 2, 8))


def run_qemu(fast, img, log):
    if os.path.exists(log):
        os.unlink(log)
    p = subprocess.Popen([
        QEMU, "-machine", "pc", "-cpu", "pentium3,+sse2,sse-fast=%s,simd-fast=%s" % (fast, fast), "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-fda", img, "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
    ])
    t0 = time.time()
    try:
        while time.time() - t0 < 900:
            time.sleep(1)
            if p.poll() is not None:
                break
            if os.path.exists(log):
                with open(log, "rb") as f:
                    f.seek(0, 2)
                    n = f.tell()
                    f.seek(max(0, n - 16))
                    if b"DONE" in f.read():
                        break
        else:
            raise SystemExit("timeout waiting for DONE (sse-fast=%s)" % fast)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    return log


def main():
    x87.ensure_prereqs()
    x87.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    asm = os.path.join(OUT, "ssetest.asm")
    com = os.path.join(OUT, "SSETEST.COM")
    with open(asm, "w") as f:
        f.write(build_asm())
    x87.sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    basm = os.path.join(OUT, "ssebench.asm")
    bcom = os.path.join(OUT, "SSEBENCH.COM")
    with open(basm, "w") as f:
        f.write(BENCH_ASM)
    x87.sh("nasm", "-O0", "-f", "bin", "-o", bcom, basm)

    img = os.path.join(OUT, "ssetest.img")
    shutil.copy(FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nSSEBENCH.COM\r\nSSETEST.COM\r\n")
    x87.sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    x87.sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    x87.sh("mcopy", "-o", "-i", img, com, "::SSETEST.COM")
    x87.sh("mcopy", "-o", "-i", img, bcom, "::SSEBENCH.COM")

    logs = {}
    for fast in ("off", "on"):
        t0 = time.time()
        logs[fast] = run_qemu(fast, img, os.path.join(OUT, "serial-%s.log" % fast))
        print("sse-fast=%s: %.1f s" % (fast, time.time() - t0))
    a = open(logs["off"], "rb").read()
    b = open(logs["on"], "rb").read()
    ticks = {}
    for fast, data in (("off", a), ("on", b)):
        for line in data.split(b"\n"):
            if line.startswith(b"SSEBENCH"):
                tag, it, tk = line.split()
                ticks[(tag.decode(), fast)] = (int(it, 16), int(tk, 16))
    for tag, what in (("SSEBENCH", "packed, 8 ops, registers"), ("SSEBENCHS", "scalar, 7 ops, registers"),
                      ("SSEBENCHM", "MMX, 8 ops, registers (simd-fast)")):
        if (tag, "off") in ticks and (tag, "on") in ticks:
            off, on = ticks[(tag, "off")], ticks[(tag, "on")]
            ratio = off[1] / max(1, on[1])
            print("bench %s: %d iterations: off %.2f s, on %.2f s (%.1fx)"
                  % (what, off[0], off[1] / 18.2, on[1] / 18.2, ratio))
            if ratio < 1.5:
                print("WARNING: the fast path does not seem active (%s): stale "
                      "build/qemu/qemu-system-i386? re-run scripts/prepare-qemu.sh, "
                      "configure, ninja" % what)
    a = b"\n".join(l for l in a.split(b"\n") if not l.startswith(b"SSEBENCH"))
    b = b"\n".join(l for l in b.split(b"\n") if not l.startswith(b"SSEBENCH"))
    n_lines = a.count(b"\n")
    if a == b and b"DONE" in a:
        print("sse guest test: %d result lines identical with the fast path on and off" % n_lines)
        return 0
    al, bl = a.split(b"\n"), b.split(b"\n")
    diffs = [(i, x, y) for i, (x, y) in enumerate(zip(al, bl)) if x != y]
    print("sse guest test: MISMATCH, %d differing lines of %d" % (len(diffs), n_lines))
    for i, x, y in diffs[:20]:
        print("  line %d: off=%s on=%s" % (i, x.decode(), y.decode()))
    return 1


if __name__ == "__main__":
    sys.exit(main())
