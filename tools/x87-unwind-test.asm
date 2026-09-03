; x87 shadow-double unwind test (patch 06, doc 13). i386 Linux user mode,
; no libc: a chain of inlined x87 instructions leaves two dirty shadows,
; then a store to an unmapped address faults. The SIGSEGV handler prints
; the FPU state the kernel (qemu-user) saved in the signal frame: status
; word, ST0 and ST1 as 80-bit values. The output must be identical with
; -cpu pentium3 and -cpu pentium3,x87-fast=off, which proves that
; x86_restore_state_to_opc() materialized the shadows during the unwind.
;
;   nasm -f elf32 -o t.o tools/x87-unwind-test.asm && ld -m elf_i386 -o t t.o
;   qemu-i386 -cpu pentium3 ./t; qemu-i386 -cpu pentium3,x87-fast=off ./t
;   (expected: 00003020 00003fff800000d6bf94d800 000040018a8f5d116cd3b000)
;
; qemu-i386 is not part of the project build; configure a scratch tree with
; --target-list=i386-linux-user,i386-softmmu -Db_staticpic=true
; --extra-cflags=-fPIC (the embed patch needs PIC, the 3dfx patch needs a
; system target for SDL detection) and build the qemu-i386 target only.
bits 32
global _start
section .text
_start:
    ; rt_sigaction(SIGSEGV, &act, NULL, 8)
    mov eax, 174
    mov ebx, 11
    mov ecx, act
    xor edx, edx
    mov esi, 8
    int 0x80

    fninit
    fldcw [cw]              ; PC=53, everything masked: inline mode
    fld qword [a]
    fmul qword [b]
    fadd qword [c]
    fld qword [d]
    fmul st1, st0           ; ST1 = (a*b+c)*d, ST0 = d, both dirty
    fstp qword [0]          ; SIGSEGV before the pop
    mov eax, 1
    mov ebx, 99
    int 0x80

handler:                    ; [esp+4] = sig, [esp+8] = struct sigcontext
    mov esi, [esp + 8 + 76] ; sc.fpstate -> struct _fpstate (fsave layout)
    movzx eax, word [esi + 4]   ; sw
    call puthex32
    mov al, ' '
    call putc
    movzx eax, word [esi + 28 + 8]   ; ST0: exponent/sign
    call puthex32
    mov eax, [esi + 28 + 4]
    call puthex32
    mov eax, [esi + 28]
    call puthex32
    mov al, ' '
    call putc
    movzx eax, word [esi + 38 + 8]   ; ST1
    call puthex32
    mov eax, [esi + 38 + 4]
    call puthex32
    mov eax, [esi + 38]
    call puthex32
    mov al, 10
    call putc
    mov eax, 1
    xor ebx, ebx
    int 0x80

putc:
    mov [hex], al
    mov eax, 4
    mov ebx, 1
    mov ecx, hex
    mov edx, 1
    int 0x80
    ret

puthex32:                   ; eax -> 8 hex digits on stdout
    push esi
    mov esi, hex + 7
    mov ecx, 8
.l: mov edx, eax
    and edx, 15
    mov dl, [digits + edx]
    mov [esi], dl
    dec esi
    shr eax, 4
    loop .l
    mov eax, 4
    mov ebx, 1
    mov ecx, hex
    mov edx, 8
    int 0x80
    pop esi
    ret

section .data
a:  dq 1.1
b:  dq 3.3
c:  dq 0.7
d:  dq 1.0000001
cw: dw 0x27f
act:
    dd handler              ; sa_handler
    dd 0                    ; sa_flags
    dd 0                    ; sa_restorer
    dq 0                    ; sa_mask
digits: db "0123456789abcdef"
section .bss
hex: resb 8
