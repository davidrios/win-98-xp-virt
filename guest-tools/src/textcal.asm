; textcal.asm - the 720x400 text-mode calibration patterns (doc 09).
;
;   TEXTCAL.COM        DOS real mode; FreeDOS on the rig, or a Win98
;                      "Restart in MS-DOS mode" screen.
;
; Windows 98 cannot put its desktop at 720x400: no display driver offers it
; and it is not a VESA graphics mode. 720x400 is the VGA *text* mode, and it
; is what the tube is already showing whenever the machine is at a DOS
; prompt - 80x25 cells of 9x16 pixels, 400 lines, 70 Hz. So the only way to
; photograph it is from DOS, which is what this is.
;
; The patterns are built from a custom character generator: a calibration
; pattern is periodic, and 256 glyphs of 9x16 tile one exactly. Two things
; here exist nowhere else:
;
;   * pattern 2, the 9th column. A VGA text cell is 9 pixels wide but the
;     glyph is 8; for character codes 0xC0-0xDF the 9th column repeats the
;     8th, and for every other code it is background. Filling half the
;     screen with a solid glyph below 0xC0 and half with the built-in block
;     at 0xDB shows the same intent coming out as stripes on one side and
;     continuous white on the other. That is what "9-dot characters" means.
;
;   * pattern 6 against pattern 1. 720x400 is scanned once; 320x200 is
;     double-scanned. The same one-on-one-off line pattern therefore repeats
;     every 2 lines on the tube in text mode and every 4 in mode 13h. Two
;     photographs at one camera setting settle doc 03 rule 3 outright.
;
;   SPACE / 1-6  pattern      ESC  quit
;
; nasm -f bin -o TEXTCAL.COM textcal.asm
;
; SPDX-License-Identifier: GPL-2.0-or-later

        cpu     386
        bits    16
        org     0x100

; The glyphs go at 0xC0.., inside the range whose 9th column repeats the 8th,
; so a horizontal line runs across the cell boundary instead of being cut
; every 9 pixels. G_SOLID8 is the same solid bitmap loaded *below* that
; range, which is the whole point of pattern 2.
NGLYPH  equ     8
G_HLINE2 equ    0xc0                    ; a lit line every 2 rows
G_HLINE4 equ    0xc1                    ; every 4
G_HLINE8 equ    0xc2                    ; every 8
G_SOLID equ     0xc3                    ; solid, 9th column repeated
G_VBAR2 equ     0xc4                    ; vertical bars, period 2
G_VBAR4 equ     0xc5                    ; period 4
G_VBAR8 equ     0xc6                    ; period 8
G_VLINE equ     0xc7                    ; one lit column per cell
G_SOLID8 equ    0x80                    ; solid below 0xC0: 9th column dark
BLOCK   equ     0xDB                    ; the built-in block: 9th col repeats

start:
        mov     [pattern], byte 0
.show:
        call    draw
        xor     ax, ax                  ; INT 16h AH=0: wait for a key
        int     0x16
        cmp     al, 0x1b
        je      .quit
        cmp     al, ' '
        je      .next
        cmp     al, '1'
        jb      .show
        cmp     al, '6'
        ja      .show
        sub     al, '1'
        mov     [pattern], al
        jmp     .show
.next:
        mov     al, [pattern]
        inc     al
        cmp     al, 6
        jb      .store
        xor     al, al
.store:
        mov     [pattern], al
        jmp     .show
.quit:
        mov     ax, 0x0003              ; back to a normal text screen
        int     0x10
        mov     ax, 0x4c00
        int     0x21

; ---------------------------------------------------------------- drawing

draw:
        mov     al, [pattern]
        cmp     al, 5
        je      .mode13
        call    set_text
        movzx   bx, byte [pattern]
        shl     bx, 1
        jmp     [text_tab + bx]
.mode13:
        jmp     pat_13h

set_text:
        mov     ax, 0x0003              ; 720x400, 80x25 of 9x16
        int     0x10
        push    cs                      ; load our glyphs at 0x80..
        pop     es
        mov     bp, glyphs
        mov     ax, 0x1100
        mov     bh, 16                  ; bytes per character
        mov     bl, 0                   ; block 0
        mov     cx, NGLYPH
        mov     dx, G_HLINE2
        int     0x10
        mov     bp, glyph_solid         ; the same solid bitmap below 0xC0
        mov     ax, 0x1100
        mov     bh, 16
        mov     bl, 0
        mov     cx, 1
        mov     dx, G_SOLID8
        int     0x10
        mov     ah, 0x01                ; cursor off
        mov     cx, 0x2000
        int     0x10
        mov     ax, 0xb800
        mov     es, ax
        ret

; fill screen rows [al, al+ah) with character bl, attribute bh
fill_rows:
        push    ax
        push    cx
        push    di
        movzx   di, al
        imul    di, di, 160
        movzx   cx, ah
        imul    cx, cx, 80
        mov     al, bl
        mov     ah, bh
.loop:
        stosw
        loop    .loop
        pop     di
        pop     cx
        pop     ax
        ret

; fill columns [cl, cl+ch) of rows [al, al+ah) with character bl, attr bh
fill_box:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di
        movzx   dx, ah                  ; dx = rows left
.row:
        movzx   di, al
        imul    di, di, 160
        movzx   bx, cl
        shl     bx, 1
        add     di, bx
        push    cx
        movzx   cx, ch
        push    ax
        mov     ax, bp                  ; bp holds char|attr<<8
.col:
        stosw
        loop    .col
        pop     ax
        pop     cx
        inc     al
        dec     dx
        jnz     .row
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; --- 1: the scanline pitch, five bands down the screen
pat_lines:
        mov     bh, 0x0f                ; white on black
        mov     bl, G_HLINE2
        mov     ax, 0x0500              ; rows 0..4
        call    fill_rows
        mov     bl, G_HLINE4
        mov     ax, 0x0505
        call    fill_rows
        mov     bl, G_HLINE8
        mov     ax, 0x050a
        call    fill_rows
        mov     bl, G_SOLID
        mov     ax, 0x050f
        call    fill_rows
        mov     bl, ' '
        mov     ax, 0x0514
        call    fill_rows
        mov     si, msg_lines
        jmp     label_it

; --- 2: the 9th column, the same solid glyph either side of 0xC0
pat_ninth:
        mov     bh, 0x0f
        mov     bl, ' '
        mov     ax, 0x1900
        call    fill_rows
        mov     bp, 0x0f00 | G_SOLID8   ; left half: 9th column is background
        mov     ax, 0x1600
        mov     cx, 0x2800              ; cols 0..39
        call    fill_box
        mov     bp, 0x0f00 | BLOCK      ; right half: 9th column repeats col 8
        mov     ax, 0x1600
        mov     cx, 0x2828
        call    fill_box
        mov     si, msg_ninth
        jmp     label_it

; --- 3: vertical bar pitch, for the horizontal spot
pat_bars:
        mov     bh, 0x0f
        mov     bl, G_VBAR2
        mov     ax, 0x0600
        call    fill_rows
        mov     bl, G_VBAR4
        mov     ax, 0x0606
        call    fill_rows
        mov     bl, G_VBAR8
        mov     ax, 0x060c
        call    fill_rows
        mov     bl, G_VLINE
        mov     ax, 0x0612
        call    fill_rows
        mov     bl, ' '
        mov     ax, 0x0118
        call    fill_rows
        mov     si, msg_bars
        jmp     label_it

; --- 4: flat fields for the mask macro (the block glyph: no 9px seam)
pat_flat:
        mov     bl, BLOCK
        mov     bh, 0x0f                ; white
        mov     ax, 0x0600
        call    fill_rows
        mov     bh, 0x07                ; light grey
        mov     ax, 0x0606
        call    fill_rows
        mov     bh, 0x04                ; red
        mov     ax, 0x040c
        call    fill_rows
        mov     bh, 0x02                ; green
        mov     ax, 0x0410
        call    fill_rows
        mov     bh, 0x01                ; blue
        mov     ax, 0x0414
        call    fill_rows
        mov     si, msg_flat
        jmp     label_it

; --- 5: real characters, which is what the mode is for
pat_text:
        mov     bh, 0x07
        mov     bl, ' '
        mov     ax, 0x1900
        call    fill_rows
        mov     si, sample
        xor     di, di
.line:
        lodsb
        or      al, al
        jz      .done
        cmp     al, 10
        jne     .put
        mov     ax, di                  ; next row
        xor     dx, dx
        mov     cx, 160
        div     cx
        inc     ax
        imul    di, ax, 160
        jmp     .line
.put:
        mov     ah, 0x07
        stosw
        jmp     .line
.done:
        mov     si, msg_text
        jmp     label_it

; --- 6: mode 13h, the same line pattern on a double-scanned mode
pat_13h:
        mov     ax, 0x0013              ; 320x200x8
        int     0x10
        mov     ax, 0xa000
        mov     es, ax
        xor     di, di
        xor     dx, dx                  ; dx = y
.row:
        mov     al, 0x0f                ; white in the default palette
        test    dl, 1                   ; band 0: a line every 2 rows
        jz      .lit
        xor     al, al
.lit:
        cmp     dx, 50
        jb      .fill
        mov     al, 0x0f                ; band 1: every 4
        test    dl, 3
        jz      .fill2
        xor     al, al
.fill2:
        cmp     dx, 100
        jb      .fill
        mov     al, 0x0f                ; band 2: every 8
        test    dl, 7
        jz      .fill3
        xor     al, al
.fill3:
        cmp     dx, 150
        jb      .fill
        mov     al, 0x0f                ; band 3: solid
.fill:
        mov     cx, 320
        rep     stosb
        inc     dx
        cmp     dx, 200
        jb      .row
        ret

; the pattern's name along the bottom row, dim, so a photograph says what it
; is; macro shots frame the middle and never see it
label_it:
        mov     di, 24 * 160
        mov     ah, 0x08                ; dark grey on black
.loop:
        lodsb
        or      al, al
        jz      .done
        stosw
        jmp     .loop
.done:
        ret

; ------------------------------------------------------------------- data

text_tab:
        dw      pat_lines, pat_ninth, pat_bars, pat_flat, pat_text

pattern db      0

msg_lines db    '1 scanline pitch 2/4/8/solid - macro a band; compare with 6', 0
msg_ninth db    '2 the 9th column: left glyph <C0 (blank), right DB (repeat)', 0
msg_bars  db    '3 vertical bars 2/4/8 and one column - horizontal spot size', 0
msg_flat  db    '4 flat fields - macro with a ruler for the mask pitch', 0
msg_text  db    '5 real characters at 9x16 - what the mode is actually for', 0

sample  db      'The quick brown fox jumps over the lazy dog 0123456789', 10
        db      'ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz', 10
        db      "!#$%&'()*+,-./:;<=>?@[]^_{|}~ 1lI0O 8B 5S 2Z", 10, 10
        db      'A VGA text cell is 9 dots wide and 16 high, so this screen', 10
        db      'is 720x400 and the tube scans 400 lines once - not the 200', 10
        db      'twice that mode 13h gives you. Photograph this and pattern', 10
        db      '6 at the same exposure and the difference is the answer.', 0

        align   16
glyphs:
        times 8 db 0xff, 0x00                   ; 0x80 every 2 rows
        times 4 db 0xff, 0x00, 0x00, 0x00       ; 0x81 every 4
        db      0xff                            ; 0x82 every 8
        times 7 db 0x00
        db      0xff
        times 7 db 0x00
glyph_solid:
        times 16 db 0xff                        ; solid
        times 16 db 0xaa                        ; 0x84 bars, period 2
        times 16 db 0xcc                        ; 0x85 period 4
        times 16 db 0xf0                        ; 0x86 period 8
        times 16 db 0x80                        ; 0x87 one column
