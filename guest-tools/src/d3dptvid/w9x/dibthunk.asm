;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; dibthunk.asm - the drawing half of the Win98/Me display driver (doc 19,
; M10). Every GDI drawing entry this driver exports is the DIB Engine's,
; reached by a jump: the driver accelerates nothing on purpose, exactly as
; the XP driver's M7a step did, and the frame buffer the Engine draws into
; is guest VRAM itself.
;
; Two shapes of entry. The plain ones take the same arguments the Engine
; does and are a bare jump. The Ext ones take one extra argument - this
; device's PDEVICE - so the thunk pops the 16:16 return address into ECX,
; pushes the extra argument, pushes the return address back and jumps;
; AX, ECX and ES are free because the Pascal convention passes nothing in
; them.
;
; SPDX-License-Identifier: GPL-2.0-or-later
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

DIBTHK  macro   name, param
extrn   DIB_&name&Ext : far
public  name
name:
        mov     ax, DGROUP
        mov     es, ax
        assume  es:DGROUP
        pop     ecx                     ; save the 16:16 return address
        push    param                   ; the extra argument
        push    ecx                     ; and the return address back
        jmp     DIB_&name&Ext
        endm

DIBFWD  macro   name
extrn   DIB_&name : far
public  name
name:
        jmp     DIB_&name
        endm

_DATA   segment public 'DATA'
extrn   _lpDriverPDevice : dword
extrn   _wPalettized : word
_DATA   ends

DGROUP  group   _DATA

_TEXT   segment public 'CODE'
.386
assume  ds:nothing, es:nothing

; the entries that need this PDEVICE passed along
DIBTHK  EnumObj,              _lpDriverPDevice
DIBTHK  RealizeObject,        _lpDriverPDevice
DIBTHK  DibBlt,               _wPalettized
DIBTHK  SetPalette,           _lpDriverPDevice
DIBTHK  GetPalette,           _lpDriverPDevice
DIBTHK  SetPaletteTranslate,  _lpDriverPDevice
DIBTHK  GetPaletteTranslate,  _lpDriverPDevice
DIBTHK  UpdateColors,         _lpDriverPDevice
DIBTHK  SetCursor,            _lpDriverPDevice
DIBTHK  MoveCursor,           _lpDriverPDevice
DIBTHK  CheckCursor,          _lpDriverPDevice

; ExtTextOut is **not** one of the thunked ones, and the reason is worth
; keeping: `DIB_ExtTextOutExt` (ordinal 403) does not take this device as
; its extra argument the way every other `…Ext` entry does — it takes *two*
; more pointers, `lpDrawTextBitmap` and `lpDrawRect`. Thunking it with one
; dword leaves the whole argument list four bytes low, and the Engine's very
; first instruction, `lds si,[bp+0x32]`, then loads a garbage selector: a
; fatal exception 0D the moment anything draws text, which on this adapter
; is a message written in VGA text mode that nothing on screen shows
; (doc 19 Section 15). The plain entry (ordinal 14) pushes the two nulls
; itself, so forwarding to it is both correct and what the reference driver
; does.
;
; and the ones that are the Engine's unchanged
DIBFWD  ExtTextOut
DIBFWD  BitBlt
DIBFWD  ColorInfo
DIBFWD  EnumDFonts
DIBFWD  Output
DIBFWD  Pixel
DIBFWD  StrBlt
DIBFWD  ScanLR
DIBFWD  DeviceMode
DIBFWD  GetCharWidth
DIBFWD  DeviceBitmap
DIBFWD  FastBorder
DIBFWD  SetAttribute
DIBFWD  CreateDIBitmap
DIBFWD  DibToDevice
DIBFWD  StretchBlt
DIBFWD  StretchDIBits
DIBFWD  SelectBitmap
DIBFWD  BitmapBits
DIBFWD  Inquire

_TEXT   ends

        end
