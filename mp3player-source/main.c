// MP3 player for Nintendo DS / DSi (homebrew .nds) - made by koi
// Plays every .mp3 in the same folder as the .nds file.
//
// Controls:
//   A              play / pause / resume
//   B              restart current track
//   Right or R     next track
//   Left  or L     previous track
//   Y or Select    cycle loop mode: ALL -> ONE -> OFF
//   Touch a song   on the bottom screen to play it
//
// Needs minimp3.h (CC0) in the same folder: https://github.com/lieff/minimp3

#include <nds.h>
#include <fat.h>
#include <maxmod9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define MAX_FILES   256
#define NAME_LEN    256
#define IN_BUF_SIZE (16 * 1024)
#define REFILL_AT   4096
#define STREAM_LEN  4096
#define LIST_ROWS   21
#define BAR_WIDTH   28

enum { LOOP_NONE = 0, LOOP_ALL = 1, LOOP_ONE = 2 };

static PrintConsole topCon, botCon;

static char dirpath[NAME_LEN];
static char files[MAX_FILES][NAME_LEN];
static int  nfiles = 0;
static int  cur = 0;

static FILE *fp = NULL;
static mp3dec_t dec;
static uint8_t inbuf[IN_BUF_SIZE];
static int inlen = 0;
static int at_eof = 0;

static short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int pcm_frames = 0;
static int pcm_pos = 0;
static int channels = 2;
static int hz = 44100;
static int bitrate = 128;
static long file_size = 0, data_start = 0;
static unsigned total_sec = 0;

static volatile int track_ended = 0;
static int stream_open = 0;
static int playing = 0;
static int paused = 0;
static int loop_mode = LOOP_ALL;   // loop is on by default
static unsigned long long samples_out = 0;

static mm_stream stream;

// ---------------------------------------------------------------- decoding

static void refill(void)
{
    if (at_eof || inlen >= REFILL_AT) return;
    size_t n = fread(inbuf + inlen, 1, IN_BUF_SIZE - inlen, fp);
    if (n == 0) at_eof = 1;
    inlen += (int)n;
}

// Decode the next MP3 frame into pcm[]. Returns 1 on success, 0 at end of file.
static int decode_next(void)
{
    for (;;) {
        refill();
        if (inlen == 0) return 0;

        mp3dec_frame_info_t info;
        int s = mp3dec_decode_frame(&dec, inbuf, inlen, pcm, &info);

        if (info.frame_bytes > 0) {
            memmove(inbuf, inbuf + info.frame_bytes, inlen - info.frame_bytes);
            inlen -= info.frame_bytes;
        } else {
            if (at_eof) return 0;
            if (inlen >= REFILL_AT) {           // junk: skip a byte and resync
                memmove(inbuf, inbuf + 1, --inlen);
            }
            continue;
        }

        if (s > 0) {
            channels = info.channels;
            hz = info.hz;
            if (info.bitrate_kbps > 0) bitrate = info.bitrate_kbps;
            pcm_frames = s;
            pcm_pos = 0;
            return 1;
        }
    }
}

// maxmod calls this (from mmStreamUpdate) whenever it needs more audio.
static mm_word stream_cb(mm_word length, mm_addr dest, mm_stream_formats format)
{
    (void)format;
    short *out = (short *)dest;
    mm_word done = 0;

    if (!paused) {
        while (done < length) {
            if (pcm_pos >= pcm_frames) {
                if (!decode_next()) { track_ended = 1; break; }
            }
            while (done < length && pcm_pos < pcm_frames) {
                if (channels == 2) {
                    out[2 * done]     = pcm[2 * pcm_pos];
                    out[2 * done + 1] = pcm[2 * pcm_pos + 1];
                } else {
                    out[2 * done] = out[2 * done + 1] = pcm[pcm_pos];
                }
                done++;
                pcm_pos++;
            }
        }
        samples_out += done;
    }

    for (mm_word i = done; i < length; i++)
        out[2 * i] = out[2 * i + 1] = 0;

    return length;
}

// ---------------------------------------------------------------- tracks

static void close_track(void)
{
    if (stream_open) { mmStreamClose(); stream_open = 0; }
    if (fp) { fclose(fp); fp = NULL; }
    playing = 0;
}

static int open_track(int index)
{
    close_track();
    if (nfiles == 0) return 0;

    char path[NAME_LEN * 2];
    size_t dl = strlen(dirpath);
    snprintf(path, sizeof path, "%s%s%s", dirpath,
             (dl > 0 && dirpath[dl - 1] == '/') ? "" : "/", files[index]);

    fp = fopen(path, "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Skip an ID3v2 tag (embedded cover art can be huge)
    uint8_t hdr[10];
    data_start = 0;
    if (fread(hdr, 1, 10, fp) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        data_start = 10 + (((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                           ((hdr[8] & 0x7F) << 7)  |  (hdr[9] & 0x7F));
    }
    fseek(fp, data_start, SEEK_SET);

    mp3dec_init(&dec);
    inlen = 0;
    at_eof = 0;
    pcm_frames = pcm_pos = 0;
    track_ended = 0;
    samples_out = 0;
    paused = 0;

    // Decode one frame first so we know the sample rate and bitrate
    if (!decode_next()) { fclose(fp); fp = NULL; return 0; }

    total_sec = bitrate > 0
        ? (unsigned)(((file_size - data_start) * 8) / ((long)bitrate * 1000))
        : 0;

    stream.sampling_rate = hz;
    stream.buffer_length = STREAM_LEN;
    stream.callback      = stream_cb;
    stream.format        = MM_STREAM_16BIT_STEREO;
    stream.timer         = MM_TIMER0;
    stream.manual        = true;
    mmStreamOpen(&stream);

    stream_open = 1;
    playing = 1;
    cur = index;
    return 1;
}

static void next_track(int dir)
{
    if (nfiles == 0) return;
    int i = (cur + dir + nfiles) % nfiles;
    for (int tries = 0; tries < nfiles; tries++) {   // skip unreadable files
        if (open_track(i)) return;
        i = (i + dir + nfiles) % nfiles;
    }
}

// ---------------------------------------------------------------- files

static int cmp_names(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void scan_dir(void)
{
    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nfiles < MAX_FILES) {
        size_t n = strlen(e->d_name);
        if (n > 4 && n < NAME_LEN && strcasecmp(e->d_name + n - 4, ".mp3") == 0) {
            strcpy(files[nfiles++], e->d_name);
        }
    }
    closedir(d);
    qsort(files, nfiles, NAME_LEN, cmp_names);
}

// ---------------------------------------------------------------- UI

// Song name without ".mp3", shortened with ".." if longer than width.
static void fmt_name(char *dst, const char *src, int width)
{
    char tmp[NAME_LEN];
    strncpy(tmp, src, NAME_LEN - 1);
    tmp[NAME_LEN - 1] = 0;
    size_t n = strlen(tmp);
    if (n > 4) tmp[n - 4] = 0;
    if ((int)strlen(tmp) > width) {
        tmp[width - 2] = 0;
        strcat(tmp, "..");
    }
    strcpy(dst, tmp);
}

static int list_first(void)
{
    int first = cur - LIST_ROWS / 2;
    if (first + LIST_ROWS > nfiles) first = nfiles - LIST_ROWS;
    if (first < 0) first = 0;
    return first;
}

// Bottom screen: playlist
static void draw_list(void)
{
    consoleSelect(&botCon);
    consoleClear();
    iprintf("\x1b[0;1H\x1b[36;1mPLAYLIST\x1b[39;0m  (%d)", nfiles);
    iprintf("\x1b[1;1H------------------------------");

    int first = list_first();
    for (int i = 0; i < LIST_ROWS && first + i < nfiles; i++) {
        char name[NAME_LEN];
        fmt_name(name, files[first + i], 27);
        int sel = (first + i == cur);
        iprintf("\x1b[%d;1H%s%c %s%s", 2 + i,
                sel ? "\x1b[37;1m" : "",
                sel ? '>' : ' ', name,
                sel ? "\x1b[39;0m" : "");
    }
}

// Top screen: progress line (cheap, redrawn about twice a second)
static void draw_time(void)
{
    consoleSelect(&topCon);
    unsigned sec = playing ? (unsigned)(samples_out / (unsigned)hz) : 0;
    if (total_sec > 0 && sec > total_sec) sec = total_sec;

    int fill = (total_sec > 0) ? (int)((unsigned long long)sec * BAR_WIDTH / total_sec) : 0;
    if (fill > BAR_WIDTH) fill = BAR_WIDTH;

    char bar[BAR_WIDTH + 1];
    for (int i = 0; i < BAR_WIDTH; i++) bar[i] = (i < fill) ? '#' : '-';
    bar[BAR_WIDTH] = 0;

    iprintf("\x1b[12;1H[%s]", bar);
    iprintf("\x1b[13;1H%02u:%02u / %02u:%02u      ",
            sec / 60, sec % 60, total_sec / 60, total_sec % 60);
}

// Top screen: everything else
static void draw_top(void)
{
    consoleSelect(&topCon);
    consoleClear();

    iprintf("\x1b[1;1H\x1b[36;1mMP3 PLAYER\x1b[39;0m");
    iprintf("\x1b[2;1H------------------------------");

    if (nfiles == 0) {
        iprintf("\x1b[6;1HNo .mp3 files found in:");
        iprintf("\x1b[7;1H%.30s", dirpath);
        iprintf("\x1b[23;20H\x1b[36mmade by koi\x1b[39m");
        return;
    }

    char name[NAME_LEN];
    fmt_name(name, files[cur], 30);
    iprintf("\x1b[4;1H\x1b[37;1mNOW PLAYING\x1b[39;0m");
    iprintf("\x1b[5;1H%s", name);
    iprintf("\x1b[6;1HTrack %d of %d", cur + 1, nfiles);

    iprintf("\x1b[9;1H%s", !playing ? "[]  STOPPED" : paused ? "||  PAUSED" : ">   PLAYING");
    iprintf("\x1b[10;1HLoop: %s",
            loop_mode == LOOP_ALL ? "ALL" : loop_mode == LOOP_ONE ? "ONE" : "OFF");

    iprintf("\x1b[16;1HA      play / pause");
    iprintf("\x1b[17;1HB      restart song");
    iprintf("\x1b[18;1HL / R  previous / next");
    iprintf("\x1b[19;1HY      loop mode");
    iprintf("\x1b[20;1HTouch a song to play it");

    iprintf("\x1b[23;20H\x1b[36mmade by koi\x1b[39m");

    draw_time();
}

static void redraw_all(void)
{
    draw_top();
    draw_list();
}

int main(int argc, char **argv)
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&topCon, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true,  true);
    consoleInit(&botCon, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    mm_ds_system sys;
    memset(&sys, 0, sizeof sys);
    sys.fifo_channel = FIFO_MAXMOD;
    mmInit(&sys);

    strcpy(dirpath, "/");
    if (fatInitDefault()) {
        if (argc > 0 && argv[0]) {
            char *slash = strrchr(argv[0], '/');
            if (slash) {
                size_t len = (size_t)(slash - argv[0]);
                if (len > 0 && len < NAME_LEN - 2) {
                    memcpy(dirpath, argv[0], len);
                    dirpath[len] = 0;
                    if (dirpath[len - 1] == ':') strcat(dirpath, "/");
                }
            }
        }
        scan_dir();
    }

    if (nfiles > 0) open_track(0);   // start playing right away
    redraw_all();

    int frame = 0;
    while (pmMainLoop()) {
        swiWaitForVBlank();
        mmStreamUpdate();

        scanKeys();
        u32 down = keysDown();
        int redraw = 0;

        if (down & KEY_A) {
            if (!playing) open_track(cur);
            else paused = !paused;
            redraw = 1;
        }
        if (down & KEY_B)                { open_track(cur); redraw = 1; }
        if (down & (KEY_R | KEY_RIGHT))  { next_track(+1);  redraw = 1; }
        if (down & (KEY_L | KEY_LEFT))   { next_track(-1);  redraw = 1; }
        if (down & (KEY_Y | KEY_SELECT)) { loop_mode = (loop_mode + 1) % 3; redraw = 1; }

        if ((down & KEY_TOUCH) && nfiles > 0) {
            touchPosition t;
            touchRead(&t);
            int row = t.py / 8;
            if (row >= 2 && row < 2 + LIST_ROWS) {
                int idx = list_first() + (row - 2);
                if (idx >= 0 && idx < nfiles) {
                    open_track(idx);
                    redraw = 1;
                }
            }
        }

        if (playing && track_ended) {
            track_ended = 0;
            if (loop_mode == LOOP_ONE) {
                open_track(cur);
            } else if (cur + 1 < nfiles || loop_mode == LOOP_ALL) {
                next_track(+1);
            } else {
                close_track();         // end of list, loop off
            }
            redraw = 1;
        }

        if (redraw) {
            redraw_all();
        } else if (nfiles > 0 && (++frame % 30) == 0) {
            draw_time();
        }
    }

    close_track();
    return 0;
}
