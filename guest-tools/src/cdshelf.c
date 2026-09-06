/*
 * CDSHELF.EXE — the host's disc shelf, from inside Windows 98 or XP.
 *
 * The Windows half of the in-guest disc shelf (doc 07); the DOS half is
 * guest-tools/src/cdshelf.asm and the protocol, shared by every side, is
 * cdshelf/cdshelf_proto.h. Lists the discs the host has on the shelf and
 * puts one of them in the drive:
 *
 *   CDSHELF                list the shelf
 *   CDSHELF 3              load slot 3
 *   CDSHELF E              empty the tray
 *   CDSHELF -d E:          use that drive rather than searching
 *   CDSHELF -v             the transport chatter as well
 *
 * ONE EXE FOR BOTH FAMILIES. The shelf is a vendor ATAPI command on the
 * machine's own CD-ROM drive (patch 52), so all this program needs is a
 * way to send a raw command to that drive — and the two Windows families
 * have different ones:
 *
 *   XP / 2000   SPTI: DeviceIoControl(IOCTL_SCSI_PASS_THROUGH_DIRECT) on
 *               a handle to \\.\<letter>:. Ring-3 port I/O is not an
 *               option on NT, and this is what every CD tool of the era
 *               uses.
 *   Win98 / Me  ASPI: WNASPI32.DLL, loaded at run time (it does not
 *               exist on XP, and linking it would make the EXE
 *               unloadable there). Windows 98 ships the layer itself
 *               (WNASPI32.DLL + APIX.VXD), so nothing has to be
 *               installed for this to work on a stock install.
 *
 * Which one is used is decided by the OS, not by a #ifdef, so the same
 * binary runs on both. Everything above the transport — the protocol,
 * the listing, the medium-change wait — is shared.
 *
 * Built by guest-tools/build-wrappers.sh with the msvcrt + pentium3 shim
 * every guest binary here uses (Win9x has no UCRT).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <ntddscsi.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdshelf_proto.h"

#define SENSE_LEN 32
#define CDB_LEN CDSHELF_CDB_LEN

/* send_cdb() results */
enum {
    CDB_OK = 0,
    CDB_SENSE = 1,      /* CHECK CONDITION; `sense` says what */
    CDB_FAILED = -1,    /* the transport itself failed */
};

/* data direction */
enum { DIR_NONE = 0, DIR_IN = 1 };

static FILE *lg;
static int verbose;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (lg) {
        fputs(buf, lg);
        fflush(lg);
    }
}

static void vlogf(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    if (!verbose) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf_("%s", buf);
}

typedef struct {
    unsigned char key, asc, ascq;
} Sense;

/* ------------------------------------------------------------------ ASPI */
/*
 * The ASPI structures, laid out by the specification and packed: this is
 * a binary interface to a 1990s DLL, and the compiler's own alignment
 * would silently move every field after SRB_BufPointer.
 */
#pragma pack(push, 1)
typedef struct {
    BYTE SRB_Cmd, SRB_Status, SRB_HaId, SRB_Flags;
    DWORD SRB_Hdr_Rsvd;
    BYTE HA_Count, HA_SCSI_ID;
    BYTE HA_ManagerId[16], HA_Identifier[16], HA_Unique[16];
    WORD HA_Rsvd1;
} SRB_HAInquiry;

typedef struct {
    BYTE SRB_Cmd, SRB_Status, SRB_HaId, SRB_Flags;
    DWORD SRB_Hdr_Rsvd;
    BYTE SRB_Target, SRB_Lun, SRB_DeviceType, SRB_Rsvd1;
} SRB_GDEVBlock;

typedef struct {
    BYTE SRB_Cmd, SRB_Status, SRB_HaId, SRB_Flags;
    DWORD SRB_Hdr_Rsvd;
    BYTE SRB_Target, SRB_Lun;
    WORD SRB_Rsvd1;
    DWORD SRB_BufLen;
    BYTE *SRB_BufPointer;
    BYTE SRB_SenseLen, SRB_CDBLen, SRB_HaStat, SRB_TargStat;
    void *SRB_PostProc;
    BYTE SRB_Rsvd2[20];
    BYTE CDBByte[16];
    BYTE SenseArea[SENSE_LEN];
} SRB_ExecSCSICmd;
#pragma pack(pop)

#define SC_HA_INQUIRY     0x00
#define SC_GET_DEV_TYPE   0x01
#define SC_EXEC_SCSI_CMD  0x02
#define SS_PENDING        0x00
#define SS_COMP           0x01
#define SRB_DIR_IN        0x08
#define SRB_DIR_OUT       0x10
#define SRB_EVENT_NOTIFY  0x40
#define DTYPE_CDROM       0x05
#define STATUS_CHKCOND    0x02

/*
 * ASPI32 is __cdecl, not stdcall — its exports carry no @n decoration,
 * which is why GetProcAddress finds them under their plain names. Getting
 * this wrong does not fail to link: the first SendASPI32Command returns
 * with the caller's stack four bytes out and Windows 98 kills the program
 * a moment later ("this program has performed an illegal operation"),
 * which is exactly what the first Win98 run of this program did.
 */
typedef DWORD (__cdecl *pfnGetASPI32SupportInfo)(void);
typedef DWORD (__cdecl *pfnSendASPI32Command)(void *);

static HMODULE aspi_dll;
static pfnGetASPI32SupportInfo aspi_support;
static pfnSendASPI32Command aspi_send;

/* ------------------------------------------------------------------ drive */
typedef struct {
    int aspi;               /* ASPI (Win9x) rather than SPTI (NT) */
    HANDLE h;               /* SPTI: \\.\<letter>: */
    char letter;
    BYTE ha, target, lun;   /* ASPI */
    char name[64];          /* what to print */
} Drive;

static int spti_send(Drive *d, const BYTE *cdb, void *data, int datalen,
                     int dir, Sense *sense)
{
    struct {
        SCSI_PASS_THROUGH_DIRECT p;
        ULONG pad;
        UCHAR sense[SENSE_LEN];
    } s;
    DWORD returned = 0;

    memset(&s, 0, sizeof s);
    s.p.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    s.p.CdbLength = CDB_LEN;
    s.p.SenseInfoLength = SENSE_LEN;
    s.p.SenseInfoOffset = (ULONG)((BYTE *)s.sense - (BYTE *)&s);
    s.p.DataIn = dir == DIR_IN ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_UNSPECIFIED;
    s.p.DataTransferLength = dir == DIR_IN ? datalen : 0;
    s.p.DataBuffer = dir == DIR_IN ? data : NULL;
    s.p.TimeOutValue = 30;
    memcpy(s.p.Cdb, cdb, CDB_LEN);

    if (!DeviceIoControl(d->h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &s, sizeof s,
                         &s, sizeof s, &returned, NULL)) {
        vlogf("  SPTI: DeviceIoControl failed, error %lu\n", (unsigned long)GetLastError());
        return CDB_FAILED;
    }
    if (s.p.ScsiStatus != 0) {
        sense->key = s.sense[2] & 0x0F;
        sense->asc = s.sense[12];
        sense->ascq = s.sense[13];
        vlogf("  SPTI: status %02x sense %02x/%02x/%02x\n", s.p.ScsiStatus,
              sense->key, sense->asc, sense->ascq);
        return CDB_SENSE;
    }
    return CDB_OK;
}

static int aspi_exec(Drive *d, const BYTE *cdb, void *data, int datalen,
                     int dir, Sense *sense)
{
    SRB_ExecSCSICmd srb;
    HANDLE ev;
    int i;

    ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    memset(&srb, 0, sizeof srb);
    srb.SRB_Cmd = SC_EXEC_SCSI_CMD;
    srb.SRB_HaId = d->ha;
    srb.SRB_Target = d->target;
    srb.SRB_Lun = d->lun;
    srb.SRB_Flags = (dir == DIR_IN ? SRB_DIR_IN : 0) | (ev ? SRB_EVENT_NOTIFY : 0);
    srb.SRB_BufLen = dir == DIR_IN ? datalen : 0;
    srb.SRB_BufPointer = dir == DIR_IN ? (BYTE *)data : NULL;
    srb.SRB_SenseLen = SENSE_LEN;
    srb.SRB_CDBLen = CDB_LEN;
    srb.SRB_PostProc = ev;
    memcpy(srb.CDBByte, cdb, CDB_LEN);

    /*
     * SendASPI32Command returns SS_PENDING only when the request is still
     * running; anything else means it already finished and the event will
     * never be signalled — waiting on it unconditionally would stall for
     * the whole timeout on every command.
     */
    if (aspi_send(&srb) == SS_PENDING) {
        if (ev) {
            if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0) {
                vlogf("  ASPI: the request did not complete in 30 s\n");
                CloseHandle(ev);
                return CDB_FAILED;
            }
        } else {
            for (i = 0; i < 30000 && srb.SRB_Status == SS_PENDING; i++) {
                Sleep(1);
            }
        }
    }
    if (ev) {
        CloseHandle(ev);
    }
    if (srb.SRB_Status == SS_COMP) {
        return CDB_OK;
    }
    if (srb.SRB_TargStat == STATUS_CHKCOND) {
        sense->key = srb.SenseArea[2] & 0x0F;
        sense->asc = srb.SenseArea[12];
        sense->ascq = srb.SenseArea[13];
        vlogf("  ASPI: status %02x sense %02x/%02x/%02x\n", srb.SRB_TargStat,
              sense->key, sense->asc, sense->ascq);
        return CDB_SENSE;
    }
    vlogf("  ASPI: SRB status %02x, host status %02x\n", srb.SRB_Status, srb.SRB_HaStat);
    return CDB_FAILED;
}

static int send_cdb(Drive *d, const BYTE *cdb, void *data, int datalen,
                    int dir, Sense *sense)
{
    sense->key = sense->asc = sense->ascq = 0;
    if (verbose) {
        char line[64];
        int i, n = 0;
        for (i = 0; i < CDB_LEN; i++) {
            n += snprintf(line + n, sizeof line - n, "%02x ", cdb[i]);
        }
        vlogf("  cdb %s(%d bytes %s)\n", line, datalen, dir == DIR_IN ? "in" : "none");
    }
    return d->aspi ? aspi_exec(d, cdb, data, datalen, dir, sense)
                   : spti_send(d, cdb, data, datalen, dir, sense);
}

/* ------------------------------------------------------------------ shelf */
static void cdb_list(BYTE *cdb, int alloc)
{
    memset(cdb, 0, CDB_LEN);
    cdb[0] = CDSHELF_OPCODE;
    cdb[1] = CDSHELF_SUB_LIST;
    cdb[7] = (BYTE)(alloc >> 8);
    cdb[8] = (BYTE)alloc;
}

static void cdb_load(BYTE *cdb, int slot)
{
    memset(cdb, 0, CDB_LEN);
    cdb[0] = CDSHELF_OPCODE;
    cdb[1] = CDSHELF_SUB_LOAD;
    cdb[2] = (BYTE)(slot >> 8);
    cdb[3] = (BYTE)slot;
}

static void cdb_eject(BYTE *cdb)
{
    memset(cdb, 0, CDB_LEN);
    cdb[0] = CDSHELF_OPCODE;
    cdb[1] = CDSHELF_SUB_EJECT;
}

static unsigned le16(const BYTE *p)
{
    return p[0] | (p[1] << 8);
}

/*
 * The reply is read twice: a header-only request first — which is how a
 * guest learns how many discs there are, and whether it understands the
 * protocol at all — and then one request sized for exactly that many.
 * `buf` must hold CDSHELF_LIST_HEADER_SIZE + total * entry_size.
 */
static int shelf_list(Drive *d, BYTE *buf, int buflen, int *total, int *count,
                      int *entry_size, int *loaded, Sense *sense)
{
    BYTE cdb[CDB_LEN];
    int r, want;

    cdb_list(cdb, CDSHELF_LIST_HEADER_SIZE);
    r = send_cdb(d, cdb, buf, CDSHELF_LIST_HEADER_SIZE, DIR_IN, sense);
    if (r != CDB_OK) {
        return r;
    }
    if (le16(buf + 0) != CDSHELF_PROTO_VERSION) {
        logf_("the drive speaks disc-shelf protocol %u, this program speaks %u.\n"
              "Rebuild CDSHELF.EXE from the same tree as the machine's QEMU.\n",
              le16(buf + 0), CDSHELF_PROTO_VERSION);
        return CDB_FAILED;
    }
    *entry_size = le16(buf + 2);
    *total = le16(buf + 6);
    if (*entry_size < CDSHELF_ENTRY_SIZE) {
        logf_("the drive reports %d-byte shelf entries; this program needs at least %d.\n",
              *entry_size, CDSHELF_ENTRY_SIZE);
        return CDB_FAILED;
    }
    *count = 0;
    *loaded = CDSHELF_NO_SLOT;
    if (*total == 0) {
        return CDB_OK;
    }
    want = CDSHELF_LIST_HEADER_SIZE + *total * *entry_size;
    if (want > buflen) {
        want = buflen - (buflen - CDSHELF_LIST_HEADER_SIZE) % *entry_size;
    }
    cdb_list(cdb, want);
    r = send_cdb(d, cdb, buf, want, DIR_IN, sense);
    if (r != CDB_OK) {
        return r;
    }
    *entry_size = le16(buf + 2);
    *count = le16(buf + 4);
    *total = le16(buf + 6);
    *loaded = le16(buf + 8);
    return CDB_OK;
}

/* the label is not NUL-terminated when it fills the field */
static void entry_label(const BYTE *e, char *out, int outlen)
{
    int n = e[CDSHELF_ENTRY_LABEL_LEN_OFF];

    if (n > CDSHELF_LABEL_MAX) {
        n = CDSHELF_LABEL_MAX;
    }
    if (n > outlen - 1) {
        n = outlen - 1;
    }
    memcpy(out, e + CDSHELF_ENTRY_LABEL_OFF, n);
    out[n] = 0;
}

/* ------------------------------------------------------------- the drives */
static int saw_no_shelf;    /* a CD-ROM drive answered, but has no shelf */

static int drive_has_shelf(Drive *d)
{
    BYTE buf[64], cdb[CDB_LEN];
    Sense sense;
    int r;

    cdb_list(cdb, CDSHELF_LIST_HEADER_SIZE);
    memset(buf, 0, sizeof buf);
    r = send_cdb(d, cdb, buf, CDSHELF_LIST_HEADER_SIZE, DIR_IN, &sense);
    if (r == CDB_OK) {
        return 1;
    }
    if (r == CDB_SENSE && sense.key == 5 && sense.asc == 0x20) {
        /* the drive knows nothing of the opcode, which is the right answer
         * for a machine started without a shelf — and the one case worth
         * explaining rather than reporting as "no drive found" */
        saw_no_shelf = 1;
        vlogf("  %s: a drive, but no shelf on it\n", d->name);
    }
    return 0;
}

/*
 * NT: every CD-ROM drive letter, opened through SPTI. A machine can have
 * more than one drive and only one of them is ours, so the shelf command
 * itself is the test — the same way the DOS build picks a drive.
 */
static int find_drive_spti(Drive *d, char want_letter)
{
    char letter;
    int found_any = 0;

    for (letter = 'C'; letter <= 'Z'; letter++) {
        char root[8], path[16];
        HANDLE h;

        sprintf(root, "%c:\\", letter);
        if (GetDriveTypeA(root) != DRIVE_CDROM) {
            continue;
        }
        if (want_letter && letter != want_letter) {
            continue;
        }
        found_any = 1;
        sprintf(path, "\\\\.\\%c:", letter);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            /* XP only lets an administrator open a drive for writing;
             * read access is enough for a data-in command. */
            h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        }
        if (h == INVALID_HANDLE_VALUE) {
            logf_("cannot open %s: error %lu\n", path, (unsigned long)GetLastError());
            continue;
        }
        memset(d, 0, sizeof *d);
        d->h = h;
        d->letter = letter;
        sprintf(d->name, "drive %c: (SPTI)", letter);
        if (drive_has_shelf(d)) {
            return 1;
        }
        CloseHandle(h);
        d->h = INVALID_HANDLE_VALUE;
    }
    if (!found_any) {
        logf_("no CD-ROM drive found.\n");
    }
    return 0;
}

/*
 * Win9x: WNASPI32.DLL, walked host adapter by host adapter. A drive
 * letter cannot be mapped to an ASPI address without the disk-info call,
 * which is for hard disks, so -d is not honoured here: every CD-ROM
 * device is asked for the shelf and the one that answers is ours.
 */
static int find_drive_aspi(Drive *d)
{
    DWORD info;
    int adapters, ha, target, lun;

    aspi_dll = LoadLibraryA("WNASPI32.DLL");
    if (!aspi_dll) {
        logf_("WNASPI32.DLL is not installed, so this program cannot reach the drive.\n"
              "On Windows 98 it is part of the system; on a stripped install, run the\n"
              "DOS build (CDSHELF.COM) from a DOS box instead.\n");
        return 0;
    }
    aspi_support = (pfnGetASPI32SupportInfo)(void *)GetProcAddress(aspi_dll, "GetASPI32SupportInfo");
    aspi_send = (pfnSendASPI32Command)(void *)GetProcAddress(aspi_dll, "SendASPI32Command");
    if (!aspi_support || !aspi_send) {
        logf_("WNASPI32.DLL is missing its entry points.\n");
        return 0;
    }
    info = aspi_support();
    if ((info >> 8 & 0xFF) != SS_COMP) {
        logf_("the ASPI layer did not start (status %02x).\n", (unsigned)(info >> 8 & 0xFF));
        return 0;
    }
    adapters = info & 0xFF;
    vlogf("ASPI: %d host adapter(s)\n", adapters);
    for (ha = 0; ha < adapters; ha++) {
        for (target = 0; target < 8; target++) {
            for (lun = 0; lun < 8; lun++) {
                SRB_GDEVBlock g;

                memset(&g, 0, sizeof g);
                g.SRB_Cmd = SC_GET_DEV_TYPE;
                g.SRB_HaId = (BYTE)ha;
                g.SRB_Target = (BYTE)target;
                g.SRB_Lun = (BYTE)lun;
                aspi_send(&g);
                if (g.SRB_Status != SS_COMP || g.SRB_DeviceType != DTYPE_CDROM) {
                    continue;
                }
                memset(d, 0, sizeof *d);
                d->aspi = 1;
                d->ha = (BYTE)ha;
                d->target = (BYTE)target;
                d->lun = (BYTE)lun;
                sprintf(d->name, "ASPI %d:%d:%d", ha, target, lun);
                vlogf("ASPI: CD-ROM at %s\n", d->name);
                if (drive_has_shelf(d)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int is_win9x(void)
{
    OSVERSIONINFOA v;

    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    GetVersionExA(&v);
    return v.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS;
}

/*
 * The medium change happens behind the command: the device runs it from a
 * bottom half (patch 52), and the drive then reports the ATAPI
 * medium-change dance — "no medium", then UNIT ATTENTION — before the new
 * disc can be read. `wait_medium` polls TEST UNIT READY through that until
 * the drive agrees the tray is (or is not) occupied, so nothing here
 * reports success before the drive does, and Windows sees a settled drive.
 *
 * `dots` prints progress for the command-line mode; the window passes 0.
 */
static int wait_medium(Drive *d, int want_present, int dots)
{
    BYTE cdb[CDB_LEN];
    Sense sense;
    int i;

    for (i = 0; i < 40; i++) {
        int r;

        Sleep(100);
        memset(cdb, 0, sizeof cdb);     /* TEST UNIT READY */
        r = send_cdb(d, cdb, NULL, 0, DIR_NONE, &sense);
        if (want_present && r == CDB_OK) {
            return 1;
        }
        /* an empty tray is exactly "not ready, medium not present" */
        if (!want_present && r == CDB_SENSE && sense.key == 2 && sense.asc == 0x3A) {
            return 1;
        }
        if (r == CDB_SENSE && sense.key != 2 && sense.key != 6) {
            vlogf("  drive reports %02x/%02x/%02x while settling\n",
                  sense.key, sense.asc, sense.ascq);
        }
        if (dots) {
            logf_(".");
        }
    }
    return 0;
}

/*
 * Put a disc in the drive: EMPTY IT FIRST, wait for the drive to say the
 * tray really is empty, and only then load the new one.
 *
 * Two reasons, both learned the hard way. Windows caches what it last saw
 * in the drive, and a swap it never saw as a removal leaves Explorer (and
 * MSCDEX) showing the previous disc's files — "inserting did nothing".
 * And on the device side the medium change runs from a *single* bottom
 * half (patch 52): an eject and a load issued back to back without
 * waiting collapse into one, and the one that survives is the last
 * request — so the wait between them is load-bearing, not politeness.
 *
 * `slot < 0` just empties the drive.
 */
static int cdshelf_insert(Drive *d, int slot, int dots, Sense *sense)
{
    BYTE cdb[CDB_LEN];
    int r;

    cdb_eject(cdb);
    r = send_cdb(d, cdb, NULL, 0, DIR_NONE, sense);
    if (r != CDB_OK) {
        return r;
    }
    wait_medium(d, 0, dots);
    if (slot < 0) {
        return CDB_OK;
    }
    cdb_load(cdb, slot);
    r = send_cdb(d, cdb, NULL, 0, DIR_NONE, sense);
    if (r != CDB_OK) {
        return r;
    }
    return wait_medium(d, 1, dots) ? CDB_OK : CDB_FAILED;
}

/*
 * Windows caches what it last saw in the drive. IOCTL_STORAGE_CHECK_VERIFY
 * is the "look again" every CD utility of the era ends with; without it
 * Explorer can keep showing the previous disc's files until something else
 * touches the drive. Best effort — a failure here changes nothing about
 * the disc actually being loaded.
 */
static void tell_windows(Drive *d)
{
    DWORD returned = 0;

    if (!d->aspi && d->h != INVALID_HANDLE_VALUE) {
        DeviceIoControl(d->h, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0,
                        &returned, NULL);
    }
}

static void print_sense(const Sense *s)
{
    if (s->key == 5 && s->asc == 0x20) {
        logf_("this drive has no shelf: the machine was started without one\n"
              "(the launcher passes it; plain qemu-system needs -device ide-cd,shelf=...).\n");
    } else if (s->key == 5 && s->asc == 0x24) {
        logf_("the drive refused the request.\n");
    } else if (s->key == 2 && s->asc == 0x3A) {
        logf_("the host cannot reach that disc image.\n");
    } else {
        logf_("the drive reports sense %02x/%02x/%02x.\n", s->key, s->asc, s->ascq);
    }
}

static void usage(void)
{
    logf_("usage: CDSHELF          open the disc-shelf window\n"
          "       CDSHELF list     print the discs on the host's shelf\n"
          "       CDSHELF <n>      put slot <n> in the drive\n"
          "       CDSHELF E        empty the drive\n"
          "       -d <letter>:     use that drive instead of searching (XP only)\n"
          "       -v               show the commands sent to the drive\n");
}

/* ------------------------------------------------------------- the window */
/*
 * Plain USER32: a listbox, four buttons and a status line, created in code
 * rather than from a dialog resource so this stays one .c file the mingw
 * build compiles with no .rc step. Nothing here is newer than Windows 95 —
 * no common controls, no manifest — because the same EXE has to come up on
 * a stock Win98 as on XP.
 *
 * Inserting a disc takes a second or two (the drive has to settle twice,
 * see cdshelf_insert), so it runs on a worker thread and posts the result
 * back: a window that stops painting mid-swap looks broken, and on Win9x a
 * blocked message loop is how a program gets a "not responding" reputation.
 */
#define WM_SHELF_DONE (WM_APP + 1)

#define ID_LIST    100
#define ID_INSERT  101
#define ID_EJECT   102
#define ID_REFRESH 103
#define ID_CLOSE   104

typedef struct {
    Drive *drive;
    BYTE *buf;
    int buflen, total, count, entry_size, loaded;
    HWND wnd, list, status, insert, eject, refresh;
    HANDLE thread;
    int busy;
    int op_slot;            /* what the worker is doing; <0 = eject */
    int op_result;
    Sense op_sense;
} Gui;

static Gui gui;

static void gui_status(const char *fmt, ...)
{
    va_list ap;
    char buf[256];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SetWindowTextA(gui.status, buf);
}

static void gui_enable(int on)
{
    EnableWindow(gui.insert, on);
    EnableWindow(gui.eject, on);
    EnableWindow(gui.refresh, on);
}

/* Re-read the shelf and repaint the list, keeping the selected row. */
static void gui_reload(void)
{
    Sense sense;
    int sel = (int)SendMessageA(gui.list, LB_GETCURSEL, 0, 0);
    int i, r;

    r = shelf_list(gui.drive, gui.buf, gui.buflen, &gui.total, &gui.count,
                   &gui.entry_size, &gui.loaded, &sense);
    SendMessageA(gui.list, LB_RESETCONTENT, 0, 0);
    if (r != CDB_OK) {
        SendMessageA(gui.list, LB_ADDSTRING, 0, (LPARAM) "(the drive did not answer)");
        gui_status("Could not read the shelf.");
        return;
    }
    for (i = 0; i < gui.count; i++) {
        const BYTE *e = gui.buf + CDSHELF_LIST_HEADER_SIZE + i * gui.entry_size;
        char label[CDSHELF_LABEL_MAX + 1], line[CDSHELF_LABEL_MAX + 64];

        entry_label(e, label, sizeof label);
        sprintf(line, "%s%s%s", label,
                e[CDSHELF_ENTRY_FLAGS_OFF] & CDSHELF_FLAG_LOADED ? "   [in the drive]" : "",
                e[CDSHELF_ENTRY_FLAGS_OFF] & CDSHELF_FLAG_MISSING ? "   [missing on the host]" : "");
        SendMessageA(gui.list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel < 0 || sel >= gui.count) {
        /* first time, or the shelf changed under us: show what is loaded */
        sel = gui.loaded < gui.count ? gui.loaded : 0;
    }
    SendMessageA(gui.list, LB_SETCURSEL, sel, 0);
    if (gui.total == 0) {
        gui_status("The host's shelf is empty.");
    } else if (gui.loaded < gui.count) {
        char label[CDSHELF_LABEL_MAX + 1];
        entry_label(gui.buf + CDSHELF_LIST_HEADER_SIZE + gui.loaded * gui.entry_size,
                    label, sizeof label);
        gui_status("In the drive: %s", label);
    } else {
        gui_status("%d disc%s on the shelf.", gui.total, gui.total == 1 ? "" : "s");
    }
}

static DWORD WINAPI gui_worker(LPVOID param)
{
    (void)param;
    gui.op_result = cdshelf_insert(gui.drive, gui.op_slot, 0, &gui.op_sense);
    tell_windows(gui.drive);
    PostMessageA(gui.wnd, WM_SHELF_DONE, 0, 0);
    return 0;
}

/* `slot < 0` empties the drive. */
static void gui_start(int slot)
{
    DWORD tid;

    if (gui.busy) {
        return;
    }
    if (slot >= 0) {
        const BYTE *e = gui.buf + CDSHELF_LIST_HEADER_SIZE + slot * gui.entry_size;
        char label[CDSHELF_LABEL_MAX + 1];

        if (e[CDSHELF_ENTRY_FLAGS_OFF] & CDSHELF_FLAG_MISSING) {
            gui_status("The host cannot reach that disc image.");
            return;
        }
        entry_label(e, label, sizeof label);
        gui_status("Inserting %s...", label);
    } else {
        gui_status("Emptying the drive...");
    }
    gui.op_slot = slot;
    gui.busy = 1;
    gui_enable(FALSE);
    gui.thread = CreateThread(NULL, 0, gui_worker, NULL, 0, &tid);
    if (!gui.thread) {   /* no thread: do it inline rather than not at all */
        gui_worker(NULL);
    }
}

static void gui_finished(void)
{
    if (gui.thread) {
        CloseHandle(gui.thread);
        gui.thread = NULL;
    }
    gui.busy = 0;
    gui_enable(TRUE);
    gui_reload();
    if (gui.op_result == CDB_SENSE) {
        const Sense *s = &gui.op_sense;
        if (s->key == 2 && s->asc == 0x3A) {
            gui_status("The host cannot reach that disc image.");
        } else {
            gui_status("The drive refused it (sense %02x/%02x/%02x).", s->key, s->asc, s->ascq);
        }
    } else if (gui.op_result != CDB_OK) {
        gui_status("The drive did not settle; try again.");
    }
}

static LRESULT CALLBACK gui_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SETFOCUS:
        /* a plain window keeps the focus itself, and then Tab and Enter
         * have nothing to act on: hand it to the list, which is what the
         * user is choosing from anyway */
        SetFocus(gui.list);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        /* Enter with the focus in the list: IsDialogMessage turns that
         * into the default push button, which is Insert. Esc closes. */
        case IDOK:
        case ID_INSERT:
            gui_start((int)SendMessageA(gui.list, LB_GETCURSEL, 0, 0));
            return 0;
        case ID_EJECT:
            gui_start(-1);
            return 0;
        case ID_REFRESH:
            gui_reload();
            return 0;
        case IDCANCEL:
        case ID_CLOSE:
            if (!gui.busy) {
                DestroyWindow(wnd);
            }
            return 0;
        case ID_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                gui_start((int)SendMessageA(gui.list, LB_GETCURSEL, 0, 0));
            }
            return 0;
        }
        break;
    case WM_SHELF_DONE:
        gui_finished();
        return 0;
    case WM_CLOSE:
        if (gui.busy) {   /* the worker still owns the drive handle */
            return 0;
        }
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static HWND gui_button(HWND parent, const char *text, int id, int x, int y, HFONT font,
                       int is_default)
{
    HWND b = CreateWindowA("BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP
                               | (is_default ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                           x, y, 90, 24, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(b, WM_SETFONT, (WPARAM)font, TRUE);
    return b;
}

static int run_gui(Drive *drive, BYTE *buf, int buflen)
{
    WNDCLASSA wc;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    MSG msg;
    char title[96];

    memset(&gui, 0, sizeof gui);
    gui.drive = drive;
    gui.buf = buf;
    gui.buflen = buflen;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = gui_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CdshelfWindow";
    if (!RegisterClassA(&wc)) {
        return 1;
    }
    sprintf(title, "Disc shelf - %s", drive->name);
    gui.wnd = CreateWindowA("CdshelfWindow", title,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 440, 320,
                            NULL, NULL, wc.hInstance, NULL);
    if (!gui.wnd) {
        return 1;
    }
    gui.list = CreateWindowA("LISTBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP
                                 | LBS_NOTIFY,
                             8, 8, 310, 240, gui.wnd, (HMENU)(INT_PTR)ID_LIST, NULL, NULL);
    SendMessageA(gui.list, WM_SETFONT, (WPARAM)font, TRUE);
    gui.insert = gui_button(gui.wnd, "&Insert", ID_INSERT, 326, 8, font, 1);
    gui.eject = gui_button(gui.wnd, "&Eject", ID_EJECT, 326, 40, font, 0);
    gui.refresh = gui_button(gui.wnd, "&Refresh", ID_REFRESH, 326, 72, font, 0);
    gui_button(gui.wnd, "&Close", ID_CLOSE, 326, 224, font, 0);
    gui.status = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
                               8, 254, 410, 20, gui.wnd, NULL, NULL, NULL);
    SendMessageA(gui.status, WM_SETFONT, (WPARAM)font, TRUE);

    gui_reload();
    ShowWindow(gui.wnd, SW_SHOWNORMAL);
    UpdateWindow(gui.wnd);
    SetFocus(gui.list);   /* so Enter inserts the highlighted disc straight away */
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(gui.wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    enum { MODE_LIST, MODE_LOAD, MODE_EJECT } mode = MODE_LIST;
    BYTE *buf;
    Drive drive;
    Sense sense;
    char want_letter = 0, label[CDSHELF_LABEL_MAX + 1];
    int slot = 0, total = 0, count = 0, entry_size = CDSHELF_ENTRY_SIZE;
    int loaded = CDSHELF_NO_SLOT, i, r, buflen, mode_given = 0;

    lg = fopen("cdshelf.log", "w");
    logf_("CDSHELF - the host's disc shelf\n");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            want_letter = (char)toupper((unsigned char)argv[++i][0]);
        } else if (!strcmp(argv[i], "list") || !strcmp(argv[i], "LIST")) {
            mode = MODE_LIST;
        } else if (argv[i][0] == 'e' || argv[i][0] == 'E') {
            mode = MODE_EJECT;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            mode = MODE_LOAD;
            slot = atoi(argv[i]);
        } else {
            usage();
            return 2;
        }
        /* a verb, as opposed to a flag, means the command line was meant */
        if (argv[i][0] != '-') {
            mode_given = 1;
        }
    }

    memset(&drive, 0, sizeof drive);
    drive.h = INVALID_HANDLE_VALUE;
    if (!(is_win9x() ? find_drive_aspi(&drive) : find_drive_spti(&drive, want_letter))) {
        logf_("no drive with a disc shelf found.\n");
        if (saw_no_shelf) {
            logf_("this drive has no shelf: the machine was started without one\n"
                  "(the launcher passes it; plain qemu-system needs "
                  "-device ide-cd,shelf=...).\n");
        }
        /* Without a console there is nothing to have read that, so the one
         * thing this program has to say gets a message box instead. */
        if (!mode_given) {
            MessageBoxA(NULL,
                        saw_no_shelf
                            ? "This machine's CD-ROM drive has no disc shelf.\n\n"
                              "The launcher gives its machines one; a machine started "
                              "by hand needs -device ide-cd,shelf=..."
                            : "No CD-ROM drive with a disc shelf was found.",
                        "Disc shelf", MB_OK | MB_ICONINFORMATION);
        }
        return 1;
    }
    logf_("%s\n", drive.name);

    buflen = CDSHELF_LIST_HEADER_SIZE + CDSHELF_FILE_MAX_ENTRIES * CDSHELF_ENTRY_SIZE;
    buf = (BYTE *)VirtualAlloc(NULL, buflen, MEM_COMMIT, PAGE_READWRITE);
    if (!buf) {
        logf_("out of memory.\n");
        return 1;
    }
    /* page-aligned: SPTI hands the buffer straight to the miniport, which
     * has its own alignment requirement, and a stack buffer can fail it */

    /* No verb: this was a double-click, not a command line. */
    if (!mode_given) {
        return run_gui(&drive, buf, buflen);
    }

    r = shelf_list(&drive, buf, buflen, &total, &count, &entry_size, &loaded, &sense);
    if (r == CDB_SENSE) {
        print_sense(&sense);
        return 1;
    }
    if (r != CDB_OK) {
        logf_("the drive did not answer the shelf listing.\n");
        return 1;
    }

    logf_("\n");
    for (i = 0; i < count; i++) {
        const BYTE *e = buf + CDSHELF_LIST_HEADER_SIZE + i * entry_size;

        entry_label(e, label, sizeof label);
        logf_("%3u  %s%s%s\n", le16(e), label,
              e[CDSHELF_ENTRY_FLAGS_OFF] & CDSHELF_FLAG_LOADED ? "  [in the drive]" : "",
              e[CDSHELF_ENTRY_FLAGS_OFF] & CDSHELF_FLAG_MISSING ? "  [missing on the host]" : "");
    }
    if (total == 0) {
        logf_("the shelf is empty.\n");
        return mode == MODE_LIST ? 0 : 1;
    }
    logf_("\n%d discs. CDSHELF <n> loads one, CDSHELF E empties the drive.\n", total);
    if (mode == MODE_LIST) {
        return 0;
    }

    if (mode == MODE_LOAD) {
        const BYTE *e;

        if (slot >= total) {
            logf_("no such slot.\n");
            return 1;
        }
        if (slot < count) {
            e = buf + CDSHELF_LIST_HEADER_SIZE + slot * entry_size;
            entry_label(e, label, sizeof label);
            if (e[CDSHELF_ENTRY_FLAGS_OFF] & CDSHELF_FLAG_MISSING) {
                /* the drive would refuse this too; say why here instead */
                logf_("the host cannot reach that disc image (it is marked missing above).\n");
                return 1;
            }
            logf_("loading slot %d: %s\n", slot, label);
        } else {
            logf_("loading slot %d\n", slot);
        }
        logf_("waiting for the drive");
        r = cdshelf_insert(&drive, slot, 1, &sense);
        logf_("\n");
        if (r == CDB_SENSE) {
            print_sense(&sense);
            return 1;
        }
        if (r != CDB_OK) {
            logf_("the drive did not settle after the load.\n");
            return 1;
        }
        tell_windows(&drive);
        logf_("the disc is in the drive.\n");
        return 0;
    }

    logf_("emptying the drive\n");
    r = cdshelf_insert(&drive, -1, 0, &sense);
    if (r == CDB_SENSE) {
        print_sense(&sense);
        return 1;
    }
    if (r != CDB_OK) {
        logf_("the drive did not accept the eject.\n");
        return 1;
    }
    tell_windows(&drive);
    logf_("the drive is empty.\n");
    return 0;
}
