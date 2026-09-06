# The Windows 9x display-driver interface headers

These are the 16-bit GDI display-driver and DIB Engine interface
definitions the Win98/Me half of the driver builds against (doc 19, M10).
They play the same role for `w9x/` that `../ddk/` plays for the XP
driver, and for the same reason: **the Microsoft DDK is not used here.**

`gdidefs.h`, `dibeng.h`, `minivdd.h`, `valmode.h`, `winhack.h`,
`vmm.h` (the ring-0 VMM services, structures and control messages the
mini-VDD is written against), `dibeng.def` and `dibeng.lbc` are taken
verbatim from **JHRobotics'
`vmdisp9x`** (`https://github.com/JHRobotics/vmdisp9x`, MIT, © 2022
JHRobotics, deriving from Michal Necasek's Win9x video minidriver), whose
`ddk/` directory carries no Microsoft code or copyright — they are
interface descriptions written from the published documentation. MIT is
compatible with this repository's GPL-2.0-or-later; the licence text is
in that project's `LICENSE` and the notice is kept here.

`dibeng.lbc` is an import list, not a library: `wlib` turns it into the
`DIBENG.DLL` import library, so nothing from a DDK is needed to link
against the DIB Engine.
