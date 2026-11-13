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
// MP3 decoding uses the Helix fixed-point decoder (the DS has no FPU).
// Decoding happens in the main loop into a ring buffer; the audio callback
// only copies samples out of it, so it stays tiny and fast.

#include <nds.h>
#include <fat.h>
#include <maxmod9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "mp3dec.h"

#define MAX_FILES    256
#define NAME_LEN     256
#define IN_BUF_SIZE  (16 * 1024)
#define REFILL_AT    6144
#define STREAM_LEN   8192
#define RING_FRAMES  16384            // must be a power of two
#define RING_MASK    (RING_FRAMES - 1)
#define PUMP_FRAMES  3                // MP3 frames decoded per main-loop pass
#define LIST_ROWS    21
#define BAR_WIDTH    28

enum { LOOP_NONE = 0, LOOP_ALL = 1, LOOP_ONE = 2 };

static PrintConsole topCon, botCon;

static char dirpath[NAME_LEN];
static char files[MAX_FILES][NAME_LEN];
static int  nfiles = 0;
static int  cur = 0;

static FILE *mp3f = NULL;
static HMP3Decoder hdec = NULL;
static unsigned char inbuf[IN_BUF_SIZE];
static int inpos = 0, inend = 0;
static int at_eof = 0;

static short pcm[2304];               // one MP3 frame: 2 granules * 2 ch * 576
static int pcm_frames = 0;            // sample frames in pcm[]
static int pcm_pos = 0;
static int channels = 2;
static int hz = 44100;
static int bitrate = 128000;
static long file_size = 0, data_start = 0;
static unsigned total_sec = 0;

static short ring[RING_FRAMES * 2];   // stereo ring buffer
static volatile unsigned wr = 0;      // written by the main loop only
static volatile unsigned rd = 0;      // written by the audio callback only
static volatile int decoder_done = 0;

static volatile int track_ended = 0;
static int stream_open = 0;
static int playing = 0;
static volatile int paused = 0;
static int loop_mode = LOOP_ALL;      // loop is on by default
static volatile unsigned long long samples_out = 0;

static mm_stream stream;

// ---------------------------------------------------------------- decoding

// Decode the next MP3 frame into pcm[]. Returns 1 on success, 0 at end of file.
static int decode_next(void)
{
    for (;;) {
        // top up the input buffer when it runs low
        if (!at_eof && (inend - inpos) < REFILL_AT) {
            if (inpos > 0) {
                memmove(inbuf, inbuf + inpos, inend - inpos);
                inend -= inpos;
                inpos = 0;
            }
            size_t n = fread(inbuf + inend, 1, IN_BUF_SIZE - inend, mp3f);
            if (n == 0) at_eof = 1;
            inend += (int)n;
        }

        int avail = inend - inpos;
        if (avail < 4) {
            if (at_eof) return 0;
            continue;
        }

        int off = MP3FindSyncWord(inbuf + inpos, avail);
        if (off < 0) {
            // no sync word here: throw everything away except the last 3 bytes
            inpos += avail - 3;
            if (at_eof) return 0;
            continue;
        }
        inpos += off;
        avail -= off;

        unsigned char *p = inbuf + inpos;
        int left = avail;
        int err = MP3Decode(hdec, &p, &left, pcm, 0);
        int used = avail - left;

        if (err == ERR_MP3_NONE) {
            inpos += used;
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(hdec, &fi);
            if (fi.outputSamps > 0 && fi.nChans > 0) {
                channels = fi.nChans;
                if (fi.samprate > 0) hz = fi.samprate;
                if (fi.bitrate > 0) bitrate = fi.bitrate;
                pcm_frames = fi.outputSamps / fi.nChans;
                pcm_pos = 0;
                return 1;
            }
        } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (at_eof) return 0;
            if (avail >= REFILL_AT) inpos += 1;      // stuck on a bogus header
        } else {
            inpos += (used > 0) ? used : 1;          // damaged frame: skip it
        }
    }
}

// Main loop: keep the ring buffer topped up (a few frames per call).
static void pump(void)
{
    if (!playing || decoder_done) return;

    int decoded = 0;
    for (;;) {
        // move pending samples into the ring
        while (pcm_pos < pcm_frames && (wr - rd) < RING_FRAMES) {
            unsigned i = wr & RING_MASK;
            if (channels == 2) {
                ring[2 * i]     = pcm[2 * pcm_pos];
                ring[2 * i + 1] = pcm[2 * pcm_pos + 1];
            } else {
                ring[2 * i] = ring[2 * i + 1] = pcm[pcm_pos];
            }
            pcm_pos++;
            wr++;
        }
        if (pcm_pos < pcm_frames) return;            // ring is full
        if (decoded >= PUMP_FRAMES) return;
        if ((RING_FRAMES - (wr - rd)) < 1152) return; // no room for a frame yet

        if (!decode_next()) { decoder_done = 1; return; }
        decoded++;
    }
}

// maxmod asks for audio here. Keep it tiny: just copy from the ring buffer.
static mm_word stream_cb(mm_word length, mm_addr dest, mm_stream_formats format)
{
    (void)format;
    short *out = (short *)dest;
    mm_word done = 0;

    if (!paused) {
        unsigned avail = wr - rd;
        unsigned r = rd;
        while (done < length && avail > 0) {
            unsigned i = r & RING_MASK;
            out[2 * done]     = ring[2 * i];
            out[2 * done + 1] = ring[2 * i + 1];
            r++;
            avail--;
            done++;
        }
        rd = r;
        samples_out += done;
        if (done < length && decoder_done && wr == rd) track_ended = 1;
    }

    for (mm_word i = done; i < length; i++)
        out[2 * i] = out[2 * i + 1] = 0;

    return length;
}

// ---------------------------------------------------------------- tracks

static void close_track(void)
{
    if (stream_open) { mmStreamClose(); stream_open = 0; }
    if (mp3f) { fclose(mp3f); mp3f = NULL; }
    if (hdec) { MP3FreeDecoder(hdec); hdec = NULL; }
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

    mp3f = fopen(path, "rb");
    if (!mp3f) return 0;

    fseek(mp3f, 0, SEEK_END);
    file_size = ftell(mp3f);
    fseek(mp3f, 0, SEEK_SET);

    // Skip an ID3v2 tag (embedded cover art can be huge)
    unsigned char hdr[10];
    data_start = 0;
    if (fread(hdr, 1, 10, mp3f) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        data_start = 10 + (((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                           ((hdr[8] & 0x7F) << 7)  |  (hdr[9] & 0x7F));
    }
    fseek(mp3f, data_start, SEEK_SET);

    hdec = MP3InitDecoder();
    if (!hdec) { fclose(mp3f); mp3f = NULL; return 0; }

    inpos = inend = 0;
    at_eof = 0;
    pcm_frames = pcm_pos = 0;
    wr = rd = 0;
    decoder_done = 0;
    track_ended = 0;
    samples_out = 0;
    paused = 0;

    // Decode one frame first so we know the sample rate and bitrate
    if (!decode_next()) { close_track(); return 0; }

    int br = bitrate;
    if (br < 1000) br *= 1000;
    total_sec = (br > 0) ? (unsigned)(((file_size - data_start) * 8) / br) : 0;

    // Pre-fill the ring buffer so playback starts cleanly
    playing = 1;
    for (int i = 0; i < 6; i++) pump();

    stream.sampling_rate = hz;
    stream.buffer_length = STREAM_LEN;
    stream.callback      = stream_cb;
    stream.format        = MM_STREAM_16BIT_STEREO;
    stream.timer         = MM_TIMER0;
    stream.manual        = true;
    mmStreamOpen(&stream);

    stream_open = 1;
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
#ifdef FIFO_MAXMOD
    sys.fifo_channel = FIFO_MAXMOD;
#endif
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

    redraw_all();                    // show the list before the first (slow) load
    if (nfiles > 0) open_track(0);   // start playing right away
    redraw_all();

    int frame = 0;
    while (pmMainLoop()) {
        pump();
        mmStreamUpdate();
        swiWaitForVBlank();

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
        } else if (nfiles > 0 && (++frame % 20) == 0) {
            draw_time();
        }
    }

    close_track();
    return 0;
}
