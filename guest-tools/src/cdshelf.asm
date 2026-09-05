; CDSHELF.COM — the host's disc shelf, from inside a DOS box.
;
; The DOS half of the in-guest disc shelf (doc 07; the Win98/XP half is
; guest-tools/src/cdshelf.c, the protocol cdshelf/cdshelf_proto.h, the
; device side patch 52). Lists the discs the host has on the shelf and
; puts one of them in the drive:
;
;   CDSHELF        list the shelf
;   CDSHELF 3      load slot 3
;   CDSHELF E      empty the tray
;
; WHY ASSEMBLY, AND WHY PIO. DOS has no networking worth the name and no
; way to reach any of our other devices, but it can talk to its own
; optical drive directly — the same ATAPI PACKET-over-PIO that
; tools/atapi-guest-test.py drives — and the shelf lives on that drive as
; a vendor opcode. There is no DOS C toolchain in this repo's build
; (guest-tools/build-wrappers.sh is a mingw cross build; Open Watcom and
; DJGPP pieces are skipped), so NASM it is. That also settles the reply
; format: fixed-stride entries this program walks with an index register.
;
; The constants below are the DOS copy of cdshelf/cdshelf_proto.h. Keep
; them in step with it — the header is the single source of truth and
; CDSHELF_PROTO_VERSION is checked at run time, so a mismatch says so
; instead of printing nonsense.
;
; SPDX-License-Identifier: GPL-2.0-or-later

                org     100h
                bits    16

; ---------------------------------------------------------------- protocol
PROTO_VERSION   equ     1
OPCODE          equ     0D0h
SUB_LIST        equ     0
SUB_LOAD        equ     1
SUB_EJECT       equ     2
HDR_SIZE        equ     12
ENTRY_SIZE      equ     68
LABEL_MAX       equ     64
MAX_ENTRIES     equ     256               ; CDSHELF_FILE_MAX_ENTRIES
NO_SLOT         equ     0FFFFh
FLAG_LOADED     equ     01h
FLAG_MISSING    equ     02h

; header fields, from the start of the reply
H_VERSION       equ     0
H_ENTRY_SIZE    equ     2
H_COUNT         equ     4
H_TOTAL         equ     6
H_LOADED        equ     8
; entry fields, from the start of an entry
E_SLOT          equ     0
E_FLAGS         equ     2
E_LABEL_LEN     equ     3
E_LABEL         equ     4

MODE_LIST       equ     0
MODE_LOAD       equ     1
MODE_EJECT      equ     2

; ---------------------------------------------------------------- entry point
start:
                mov     ax, cs                  ; the reply buffer: one
                add     ax, 1000h               ; segment above us, like
                mov     [bufseg], ax            ; atapi-guest-test.py's
                mov     si, str_banner
                call    puts
                call    parse_args
                jnc     .args_ok
                jmp     usage
.args_ok:
                call    find_drive
                jnc     .drive_ok
                jmp     no_drive
.drive_ok:
                call    print_drive
                call    list_shelf              ; every mode lists first:
                jnc     .list_ok                ; it validates the slot and
                jmp     shelf_failed            ; names the disc we load
.list_ok:
                call    show_list
                mov     al, [cmdmode]
                cmp     al, MODE_LIST
                je      done
                cmp     al, MODE_EJECT
                je      do_eject
                jmp     do_load

done:
                mov     ax, 4C00h
                int     21h

; ---------------------------------------------------------------- the modes
do_load:
                mov     ax, [cmdslot]
                cmp     ax, [nshelf]
                jb      .in_range
                mov     si, str_no_such_slot
                call    puts
                jmp     fail
.in_range:
                ; the host flagged this one as unreachable in the listing we
                ; just printed; the drive would refuse the load anyway, but
                ; say why here rather than after a failed command
                call    slot_flags
                test    al, FLAG_MISSING
                jz      .ok
                mov     si, str_host_missing
                call    puts
                jmp     fail
.ok:
                mov     si, str_loading
                call    puts
                mov     ax, [cmdslot]
                call    putdec
                mov     si, str_colon_sp
                call    puts
                mov     ax, [cmdslot]
                call    print_slot_label
                call    newline
                mov     byte [pkt + 1], SUB_LOAD
                mov     ax, [cmdslot]
                xchg    al, ah                  ; the slot is big-endian
                mov     [pkt + 2], ax
                xor     ax, ax
                call    set_alloc
                call    send_packet
                jnc     .sent
                jmp     cmd_failed
.sent:
                call    wait_medium
                jmp     done

do_eject:
                mov     si, str_ejecting
                call    puts
                mov     byte [pkt + 1], SUB_EJECT
                mov     word [pkt + 2], 0
                xor     ax, ax
                call    set_alloc
                call    send_packet
                jnc     .sent
                jmp     cmd_failed
.sent:
                mov     si, str_ejected
                call    puts
                jmp     done

; ---------------------------------------------------------------- the listing
; LIST twice: a header-only request first (that is how a guest learns how
; many discs there are and whether it understands the protocol at all),
; then one request sized for exactly that many entries.
list_shelf:
                mov     byte [pkt + 1], SUB_LIST
                mov     word [pkt + 2], 0
                mov     ax, HDR_SIZE
                call    set_alloc
                call    send_packet
                jc      .err
                cmp     word [total], HDR_SIZE
                jb      .short
                call    check_version
                jc      .fail
                mov     es, [bufseg]
                mov     ax, [es:H_TOTAL]
                mov     [nshelf], ax            ; discs on the shelf, which is
                test    ax, ax                  ; not [total] (bytes received)
                jz      .empty
                cmp     ax, MAX_ENTRIES
                jbe     .n_ok
                mov     ax, MAX_ENTRIES
.n_ok:
                mov     bx, ax
                mov     ax, [entstride]
                mul     bx                      ; dx:ax; the stride check above
                add     ax, HDR_SIZE            ; keeps this inside one segment
                call    set_alloc
                call    send_packet
                jc      .err
                clc
                ret
.empty:
                clc
                ret
.short:
                mov     si, str_short_reply
                call    puts
.fail:
                stc
                ret
.err:
                call    print_sense
                stc
                ret

; The version is the one thing worth refusing on: a reply laid out by a
; different version of the protocol would be walked wrongly and print
; garbage. The entry stride is then taken from the reply rather than from
; this program's own constant — that is what the field is for, and the
; fields inside an entry are at fixed offsets by design — with an upper
; bound, because the whole listing has to stay inside one 64 KB segment.
check_version:
                mov     es, [bufseg]
                cmp     word [es:H_VERSION], PROTO_VERSION
                jne     .bad_version
                mov     ax, [es:H_ENTRY_SIZE]
                cmp     ax, ENTRY_SIZE
                jb      .bad_stride
                cmp     ax, 240
                ja      .bad_stride
                mov     [entstride], ax
                clc
                ret
.bad_version:
                mov     si, str_bad_version
                call    puts
                mov     ax, [es:H_VERSION]
                call    putdec
                mov     si, str_want_version
                call    puts
                mov     ax, PROTO_VERSION
                call    putdec
                call    newline
                stc
                ret
.bad_stride:
                mov     si, str_bad_stride
                call    puts
                stc
                ret

show_list:
                call    newline
                mov     ax, [nshelf]
                test    ax, ax
                jz      .none
                mov     es, [bufseg]
                mov     cx, [es:H_COUNT]
                test    cx, cx
                jz      .none
                mov     si, HDR_SIZE            ; es:si walks the entries
.row:
                push    cx
                mov     al, ' '
                call    putc
                call    putc
                mov     ax, [es:si + E_SLOT]
                call    putdec3
                mov     al, ' '
                call    putc
                call    putc
                mov     cl, [es:si + E_LABEL_LEN]
                xor     ch, ch
                push    si
                add     si, E_LABEL
                call    print_label             ; not NUL-terminated when full
                pop     si
                mov     al, [es:si + E_FLAGS]
                test    al, FLAG_LOADED
                jz      .no_loaded
                push    si
                mov     si, str_in_drive
                call    puts
                pop     si
.no_loaded:
                mov     al, [es:si + E_FLAGS]
                test    al, FLAG_MISSING
                jz      .no_missing
                push    si
                mov     si, str_missing
                call    puts
                pop     si
.no_missing:
                call    newline
                add     si, [entstride]
                pop     cx
                loop    .row
                call    newline
                mov     ax, [nshelf]
                call    putdec
                mov     si, str_discs
                call    puts
                ret
.none:
                mov     si, str_empty_shelf
                call    puts
                ret

; -> al = the flags of [cmdslot]'s entry (0 when it is not in the listing)
slot_flags:
                mov     es, [bufseg]
                mov     ax, [cmdslot]
                cmp     ax, [es:H_COUNT]
                jae     .none
                mov     bx, [entstride]
                mul     bx
                mov     si, ax
                mov     al, [es:si + HDR_SIZE + E_FLAGS]
                ret
.none:
                xor     al, al
                ret

; ax = slot -> print that entry's label (the listing is still in the buffer)
print_slot_label:
                mov     es, [bufseg]
                cmp     ax, [es:H_COUNT]
                jae     .out
                mov     bx, [entstride]
                mul     bx
                mov     si, ax
                add     si, HDR_SIZE
                mov     cl, [es:si + E_LABEL_LEN]
                xor     ch, ch
                add     si, E_LABEL
                call    print_label
                ret
.out:
                ret

; After a LOAD the medium change happens behind the command (the device
; runs it from a bottom half, patch 52), and the drive then reports the
; ATAPI medium-change dance — "no medium", then UNIT ATTENTION — before
; the new disc can be read. Poll TEST UNIT READY through it so the user
; is told when the disc is actually there, and DOS sees a settled drive.
wait_medium:
                mov     si, str_waiting
                call    puts
                mov     cx, 40
.poll:
                push    cx
                mov     al, 2
                call    wait_ticks
                mov     byte [pkt + 0], 0       ; TEST UNIT READY
                mov     word [pkt + 1], 0
                mov     word [pkt + 3], 0
                mov     word [pkt + 7], 0
                call    send_packet
                mov     byte [pkt + 0], OPCODE
                pop     cx
                jnc     .ready
                ; 02/3A is "the tray is still empty" and 06/28 is the medium
                ; change itself; both mean "keep waiting". Anything else is a
                ; real failure and worth showing rather than spinning on.
                mov     al, [sense_key]
                cmp     al, 2
                je      .again
                cmp     al, 6
                je      .again
                call    newline
                call    print_sense
                ret
.again:
                mov     al, '.'
                call    putc
                loop    .poll
                call    newline
                mov     si, str_not_ready
                call    puts
                ret
.ready:
                call    newline
                mov     si, str_ready
                call    puts
                ret

; ---------------------------------------------------------------- errors
usage:
                mov     si, str_usage
                call    puts
                jmp     fail

no_drive:
                mov     si, str_no_drive
                call    puts
                cmp     byte [saw_atapi], 0
                je      fail
                mov     si, str_no_shelf
                call    puts
                jmp     fail

shelf_failed:
                mov     si, str_cmd_failed
                call    puts
                jmp     fail

cmd_failed:
                call    print_sense
                mov     si, str_cmd_failed
                call    puts
fail:
                mov     ax, 4C01h
                int     21h

; The drive's own account of the last failure, which is more use than a
; status byte: "no shelf on this drive" (5/20) and "the drive refused the
; request" (5/24) are the two a user can act on.
print_sense:
                mov     al, [sense_key]
                cmp     al, 0FFh
                je      .unknown
                cmp     al, 2                   ; NOT READY: for a load, the
                jne     .not_ready_done         ; host could not open the file
                mov     al, [sense_asc]
                cmp     al, 3Ah
                jne     .other
                mov     si, str_host_missing
                call    puts
                ret
.not_ready_done:
                cmp     al, 5
                jne     .other
                mov     al, [sense_asc]
                cmp     al, 20h
                jne     .not_opcode
                mov     si, str_no_shelf
                call    puts
                ret
.not_opcode:
                cmp     al, 24h
                jne     .other
                mov     si, str_refused
                call    puts
                ret
.other:
                mov     si, str_sense
                call    puts
                mov     al, [sense_key]
                call    puthex8
                mov     al, '/'
                call    putc
                mov     al, [sense_asc]
                call    puthex8
                mov     al, '/'
                call    putc
                mov     al, [sense_ascq]
                call    puthex8
                call    newline
                ret
.unknown:
                mov     si, str_sense_failed
                call    puts
                ret

; ---------------------------------------------------------------- the drive
; Probe both channels, master and slave for an ATAPI device, then ask each
; one for the shelf: a machine can have more than one drive and only one of
; them is ours.
;
; The device is found with IDENTIFY PACKET DEVICE (A1h) rather than by
; reading the ATAPI signature (14h/EBh) out of the cylinder registers: the
; signature is only there until something issues a command, and by the time
; a DOS program runs, the BIOS has long since detected the drive and left
; those registers at zero (checked under SeaBIOS: status 50h, cylinders
; 00/00). A1h is the question itself — an ATAPI device answers it with a
; data block, everything else aborts it.
find_drive:
                mov     bx, drv_tab
.next:
                mov     ax, [bx]
                test    ax, ax
                jz      .none
                mov     [base], ax
                mov     ax, [bx + 2]
                mov     [ctrl], ax
                mov     ax, [bx + 4]
                mov     [sel], al
                mov     ax, [bx + 6]
                mov     [drvname], ax
                push    bx
                call    probe_drive
                pop     bx
                jnc     .found
                add     bx, 8
                jmp     .next
.found:
                clc
                ret
.none:
                stc
                ret

probe_drive:
                call    select_drive
                mov     dx, [base]
                add     dx, 7
                in      al, dx
                cmp     al, 0FFh                ; floating bus: no channel
                je      .no
                test    al, al                  ; no device selected
                jz      .no
                call    wait_bsy_clear
                jc      .no
                mov     dx, [base]
                add     dx, 7
                mov     al, 0A1h                ; IDENTIFY PACKET DEVICE
                out     dx, al
                call    wait_drq
                jc      .no
                mov     es, [bufseg]            ; drain the block, or the drive
                xor     di, di                  ; sits in DRQ and the PACKET
                mov     cx, 256                 ; command below is refused
                mov     dx, [base]
                rep     insw
                mov     byte [saw_atapi], 1
                mov     byte [pkt + 0], OPCODE  ; does it have a shelf?
                mov     byte [pkt + 1], SUB_LIST
                mov     word [pkt + 2], 0
                mov     ax, HDR_SIZE
                call    set_alloc
                call    send_packet
                jc      .no
                cmp     word [total], HDR_SIZE
                jb      .no
                clc
                ret
.no:
                stc
                ret

select_drive:
                mov     dx, [base]
                add     dx, 6
                mov     al, [sel]
                out     dx, al
                mov     dx, [ctrl]              ; four alternate-status reads
                in      al, dx                  ; = the 400 ns settle every
                in      al, dx                  ; ATA driver does after a
                in      al, dx                  ; drive select
                in      al, dx
                ret

print_drive:
                mov     si, str_drive
                call    puts
                mov     si, [drvname]
                call    puts
                call    newline
                ret

; ---------------------------------------------------------------- ATAPI PIO
; One PACKET command: select, byte count limit, PACKET, the 12-byte CDB,
; then the DRQ loop reading whatever the drive gives us into bufseg:0.
; Carry set = CHECK CONDITION (or a timeout); [total] = bytes received.
; A CHECK CONDITION is always followed by REQUEST SENSE, exactly as a real
; driver does it: the drive keeps reporting the same condition to every
; command until something asks for the sense data, so skipping this turns
; one failure into an endless one (which is precisely what the medium-change
; poll below did before this).
send_packet:
                mov     si, pkt
                call    send_packet_raw
                jnc     .ok
                call    fetch_sense
                stc
                ret
.ok:
                clc
                ret

fetch_sense:
                mov     byte [sense_key], 0FFh
                mov     byte [sense_asc], 0
                mov     byte [sense_ascq], 0
                push    si
                mov     si, pkt_sense
                call    send_packet_raw
                pop     si
                jc      .done
                cmp     word [total], 14
                jb      .done
                mov     es, [bufseg]
                mov     al, [es:2]
                and     al, 0Fh
                mov     [sense_key], al
                mov     al, [es:12]
                mov     [sense_asc], al
                mov     al, [es:13]
                mov     [sense_ascq], al
.done:
                ret

send_packet_raw:
                mov     word [total], 0
                call    select_drive
                call    wait_bsy_clear
                jc      .timeout
                mov     dx, [ctrl]
                mov     al, 02h                 ; nIEN: we poll, no interrupts
                out     dx, al
                mov     dx, [base]
                inc     dx
                xor     al, al                  ; features: PIO
                out     dx, al
                add     dx, 3                   ; byte count limit = 0FFFEh
                mov     al, 0FEh
                out     dx, al
                inc     dx
                mov     al, 0FFh
                out     dx, al
                mov     dx, [base]
                add     dx, 7
                mov     al, 0A0h                ; PACKET
                out     dx, al
                call    wait_drq
                jc      .fail
                mov     dx, [base]
                mov     cx, 6
                rep     outsw
                mov     es, [bufseg]
                xor     di, di
.loop:
                call    wait_bsy_clear
                jc      .timeout
                mov     dx, [base]
                add     dx, 7
                in      al, dx
                test    al, 01h                 ; ERR
                jnz     .fail
                test    al, 08h                 ; DRQ
                jz      .done
                mov     dx, [base]
                add     dx, 4
                in      al, dx
                mov     cl, al
                inc     dx
                in      al, dx
                mov     ch, al
                add     [total], cx
                inc     cx
                shr     cx, 1
                mov     dx, [base]
                rep     insw
                jmp     .loop
.done:
                clc
                ret
.fail:
                stc
                ret
.timeout:
                mov     si, str_timeout
                call    puts
                stc
                ret

wait_bsy_clear:
                push    ecx
                mov     ecx, 8000000
.l:
                mov     dx, [base]
                add     dx, 7
                in      al, dx
                test    al, 80h
                jz      .ok
                dec     ecx
                jnz     .l
                pop     ecx
                stc
                ret
.ok:
                pop     ecx
                clc
                ret

wait_drq:                                       ; BSY clear and DRQ or ERR
                push    ecx
                mov     ecx, 8000000
.l:
                mov     dx, [base]
                add     dx, 7
                in      al, dx
                test    al, 80h
                jnz     .again
                test    al, 09h
                jnz     .got
.again:
                dec     ecx
                jnz     .l
                pop     ecx
                stc
                ret
.got:
                pop     ecx
                test    al, 01h
                jnz     .err
                clc
                ret
.err:
                stc
                ret

set_alloc:                                      ; ax = allocation length
                mov     [pkt + 8], al
                mov     [pkt + 7], ah
                ret

wait_ticks:                                     ; al = BIOS ticks (18.2 Hz)
                push    es
                push    cx
                movzx   cx, al
                xor     ax, ax
                mov     es, ax
.t:
                mov     ax, [es:046Ch]
.w:
                cmp     ax, [es:046Ch]
                je      .w
                loop    .t
                pop     cx
                pop     es
                ret

; ---------------------------------------------------------------- arguments
parse_args:
                mov     byte [cmdmode], MODE_LIST
                mov     si, 81h
                mov     cl, [80h]
                xor     ch, ch
.skip:
                jcxz    .none
                mov     al, [si]
                cmp     al, ' '
                je      .adv
                cmp     al, 9
                jne     .have
.adv:
                inc     si
                dec     cx
                jmp     .skip
.none:
                clc
                ret
.have:
                cmp     al, 'e'
                je      .eject
                cmp     al, 'E'
                je      .eject
                cmp     al, '0'
                jb      .bad
                cmp     al, '9'
                ja      .bad
                xor     bx, bx
.digit:
                jcxz    .digits_done
                mov     al, [si]
                cmp     al, '0'
                jb      .digits_done
                cmp     al, '9'
                ja      .digits_done
                sub     al, '0'
                push    ax
                mov     ax, bx
                mov     bx, 10
                mul     bx
                mov     bx, ax
                pop     ax
                mov     ah, 0
                add     bx, ax
                inc     si
                dec     cx
                jmp     .digit
.digits_done:
                mov     [cmdslot], bx
                mov     byte [cmdmode], MODE_LOAD
                clc
                ret
.eject:
                mov     byte [cmdmode], MODE_EJECT
                clc
                ret
.bad:
                stc
                ret

; ---------------------------------------------------------------- output
; Everything goes through DOS function 02h, so `CDSHELF > FILE` and
; `CDSHELF > COM1` work — which is how tools/atapi-guest-test.py reads
; this program's output back out of the guest.
; DOS is documented to preserve the registers a call does not return in,
; but not every DOS does, and this one is called from loops that keep the
; reply buffer in ES and the entry index in SI. Saving everything costs
; nothing here and has been the difference between a listing and a hang
; in enough era code to be worth it.
putc:
                push    ax
                push    bx
                push    cx
                push    dx
                push    si
                push    di
                push    bp
                push    ds
                push    es
                mov     dl, al
                mov     ah, 02h
                push    cs
                pop     ds
                int     21h
                pop     es
                pop     ds
                pop     bp
                pop     di
                pop     si
                pop     dx
                pop     cx
                pop     bx
                pop     ax
                ret

puts:                                           ; ds:si asciiz
                push    ax
                push    si
.l:
                lodsb
                test    al, al
                jz      .d
                call    putc
                jmp     .l
.d:
                pop     si
                pop     ax
                ret

print_label:                                    ; es:si, cx bytes
                push    ax
                push    cx
                push    si
.l:
                jcxz    .d
                mov     al, [es:si]
                inc     si
                dec     cx
                call    putc
                jmp     .l
.d:
                pop     si
                pop     cx
                pop     ax
                ret

putdec3:                                        ; ax, right-aligned in 3
                cmp     ax, 100
                jae     putdec
                push    ax
                mov     al, ' '
                call    putc
                pop     ax
                cmp     ax, 10
                jae     putdec
                push    ax
                mov     al, ' '
                call    putc
                pop     ax
putdec:
                push    ax
                push    bx
                push    cx
                push    dx
                mov     bx, 10
                xor     cx, cx
.div:
                xor     dx, dx
                div     bx
                push    dx
                inc     cx
                test    ax, ax
                jnz     .div
.emit:
                pop     ax
                add     al, '0'
                call    putc
                loop    .emit
                pop     dx
                pop     cx
                pop     bx
                pop     ax
                ret

puthex8:
                push    ax
                shr     al, 4
                call    .nib
                pop     ax
                push    ax
                call    .nib
                pop     ax
                ret
.nib:
                and     al, 0Fh
                add     al, '0'
                cmp     al, '9'
                jbe     .out
                add     al, 'a' - '0' - 10
.out:
                jmp     putc

newline:
                push    ax
                mov     al, 13
                call    putc
                mov     al, 10
                call    putc
                pop     ax
                ret

; ---------------------------------------------------------------- data
str_banner:     db      "CDSHELF - the host's disc shelf", 13, 10, 0
str_drive:      db      "drive: ", 0
str_usage:      db      "usage: CDSHELF        list the discs on the host's shelf", 13, 10
                db      "       CDSHELF <n>    put slot <n> in the drive", 13, 10
                db      "       CDSHELF E      empty the drive", 13, 10, 0
str_no_drive:   db      "no drive with a disc shelf found on either IDE channel.", 13, 10, 0
str_no_shelf:   db      "this drive has no shelf: the machine was started without one", 13, 10
                db      "(the launcher passes it; plain qemu-system needs -device ide-cd,shelf=...)", 13, 10, 0
str_refused:    db      "the drive refused the request.", 13, 10, 0
str_cmd_failed: db      "the shelf command failed.", 13, 10, 0
str_sense:      db      "sense key/asc/ascq ", 0
str_sense_failed: db    "the drive reported an error and then no sense data.", 13, 10, 0
str_timeout:    db      "the drive did not answer (timeout).", 13, 10, 0
str_short_reply: db     "the drive answered too short a reply for a shelf listing.", 13, 10, 0
str_bad_version: db     "shelf protocol version ", 0
str_bad_stride: db      "the drive's shelf entries are not a size this program can walk.", 13, 10, 0
str_want_version: db    " from the host, this program speaks ", 0
str_empty_shelf: db     "the shelf is empty.", 13, 10, 0
str_discs:      db      " discs. CDSHELF <n> loads one, CDSHELF E empties the drive.", 13, 10, 0
str_in_drive:   db      "  [in the drive]", 0
str_missing:    db      "  [missing on the host]", 0
str_no_such_slot: db    "no such slot.", 13, 10, 0
str_host_missing: db    "the host cannot reach that disc image (it is marked missing above).", 13, 10, 0
str_loading:    db      "loading slot ", 0
str_colon_sp:   db      ": ", 0
str_ejecting:   db      "emptying the drive", 13, 10, 0
str_ejected:    db      "the drive is empty.", 13, 10, 0
str_waiting:    db      "waiting for the drive", 0
str_ready:      db      "the disc is in the drive.", 13, 10, 0
str_not_ready:  db      "the drive still reports no disc.", 13, 10, 0
str_pm:         db      "primary master (1F0h)", 0
str_ps:         db      "primary slave (1F0h)", 0
str_sm:         db      "secondary master (170h)", 0
str_ss:         db      "secondary slave (170h)", 0

; base, control, drive select, name
drv_tab:        dw      1F0h, 3F6h, 00A0h, str_pm
                dw      1F0h, 3F6h, 00B0h, str_ps
                dw      170h, 376h, 00A0h, str_sm
                dw      170h, 376h, 00B0h, str_ss
                dw      0

pkt:            db      OPCODE, SUB_LIST, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
pkt_sense:      db      03h, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0

base:           dw      0
ctrl:           dw      0
sel:            db      0
drvname:        dw      0
saw_atapi:      db      0
total:          dw      0                       ; bytes in the last reply
nshelf:         dw      0                       ; discs on the shelf
entstride:      dw      ENTRY_SIZE              ; the reply's own entry size
sense_key:      db      0FFh                    ; the last CHECK CONDITION
sense_asc:      db      0
sense_ascq:     db      0
bufseg:         dw      0
cmdmode:        db      0
cmdslot:        dw      0
