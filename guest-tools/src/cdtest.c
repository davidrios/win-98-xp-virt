/*
 * CDTEST.EXE — CD audio through MCI (doc 17 §6.3): opens the cdaudio
 * device, lists the tracks, plays track 2 for a few seconds while polling
 * the position, then track 3, and logs everything to cdtest.log next to
 * the EXE. Under the player the tone is heard; headless, `-audiodev
 * wav,id=cd0,path=x.wav -device ide-cd,...,audiodev=cd0` records it.
 *
 *   CDTEST.EXE [drive letter] [seconds]      (defaults: the first CD drive, 4 s)
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

static FILE *lg;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (lg) {
        fputs(buf, lg);
        fflush(lg);
    }
}

static int mci(const char *cmd, char *out, int outlen)
{
    char ret[256];
    MCIERROR e;
    ret[0] = 0;
    e = mciSendStringA(cmd, ret, sizeof ret, NULL);
    if (e) {
        char msg[256];
        mciGetErrorStringA(e, msg, sizeof msg);
        logf_("mci \"%s\" -> error %lu: %s\n", cmd, (unsigned long)e, msg);
        return -1;
    }
    logf_("mci \"%s\" -> \"%s\"\n", cmd, ret);
    if (out) {
        strncpy(out, ret, outlen - 1);
        out[outlen - 1] = 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char cmd[256], ret[256];
    int seconds = argc > 2 ? atoi(argv[2]) : 4, tracks = 0, i;
    DWORD t0;

    lg = fopen("cdtest.log", "w");
    if (argc > 1) {
        snprintf(cmd, sizeof cmd, "open %c: type cdaudio alias cd", argv[1][0]);
    } else {
        snprintf(cmd, sizeof cmd, "open cdaudio alias cd");
    }
    if (mci(cmd, NULL, 0) < 0) {
        logf_("RESULT open failed\n");
        return 1;
    }
    mci("set cd time format tmsf", NULL, 0);
    mci("status cd media present", ret, sizeof ret);
    if (mci("status cd number of tracks", ret, sizeof ret) == 0) {
        tracks = atoi(ret);
    }
    logf_("tracks %d\n", tracks);
    for (i = 1; i <= tracks && i <= 20; i++) {
        snprintf(cmd, sizeof cmd, "status cd type track %d", i);
        mci(cmd, ret, sizeof ret);
        snprintf(cmd, sizeof cmd, "status cd length track %d", i);
        mci(cmd, ret, sizeof ret);
        snprintf(cmd, sizeof cmd, "status cd position track %d", i);
        mci(cmd, ret, sizeof ret);
    }
    if (tracks < 2) {
        logf_("RESULT no audio track to play\n");
        mci("close cd", NULL, 0);
        return 1;
    }
    if (mci("play cd from 2", NULL, 0) < 0) {
        logf_("RESULT play failed\n");
        mci("close cd", NULL, 0);
        return 1;
    }
    t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)seconds * 1000) {
        Sleep(500);
        mci("status cd position", ret, sizeof ret);
        mci("status cd mode", ret, sizeof ret);
    }
    mci("pause cd", NULL, 0);
    Sleep(300);
    mci("status cd mode", ret, sizeof ret);
    mci("status cd position", ret, sizeof ret);
    mci("resume cd", NULL, 0);
    Sleep(700);
    mci("status cd position", ret, sizeof ret);
    mci("stop cd", NULL, 0);
    mci("status cd mode", ret, sizeof ret);
    if (tracks >= 3) {
        mci("play cd from 3 to 3", NULL, 0);
        Sleep(1500);
        mci("status cd position", ret, sizeof ret);
        mci("stop cd", NULL, 0);
    }
    mci("close cd", NULL, 0);
    logf_("RESULT ok\n");
    if (lg) {
        fclose(lg);
    }
    return 0;
}
