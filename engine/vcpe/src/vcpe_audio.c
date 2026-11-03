/* VCPE audio: software mixer. IMA-ADPCM SFX voices (22.05 kHz mono, resident) + streamed
 * IMA-ADPCM music (44.1 kHz stereo) from the music PAK, output at 44.1 kHz stereo.
 * Games put their own sound policy (which voice plays what, volume curves) on top. */

/* HOW THIS FILE WORKS
 *
 * The PSP has no sound mixer the game can use freely, only output channels that play a buffer of 16-bit stereo
 * samples. So mixing is done in software, on a dedicated thread:
 *
 * - mix_thread fills a block of OUT_FRAMES stereo samples (1024 = 23 ms at 44.1 kHz), adds every playing sound effect
 *   and the music into it, then hands it to sceAudioOutputPannedBlocking, which blocks until the hardware has room.
 *   Two output buffers alternate, so one plays while the next is mixed.
 * - Sound effects are kept in memory compressed with IMA-ADPCM (4 bits per sample, a quarter of 16-bit PCM), 22.05
 *   kHz mono, and decoded as they play. Each "voice" plays one effect with its own pitch, gain and loop flag.
 * - Music is too big for memory: io_thread streams it from the music PAK into a ring buffer of 48 blocks, ahead of
 *   the mixer. The mixer only reads the ring; when it frees a block it signals io_thread to read more.
 *
 * IMA-ADPCM in brief: each 4-bit code says how much the sample changes relative to a "step size", and the step size
 * itself grows or shrinks after each code (step_table / index_table, the standard IMA tables). Data comes in blocks
 * of 1024 samples; each block starts with a header (first sample + step index) so decoding can start at any block.
 *
 * 5.6: music is decoded a whole run at a time (up to the end of the output block or of the ADPCM block) into a
 * stereo buffer, instead of one call per sample with every check repeated; the reader skips the seek when the file
 * is already at the next block; threads, semaphores and files go through vcpe_plat; vcpe_audio_ms counts exact
 * milliseconds (it gained 23 ms per 23.2 ms block before, about 1% slow). The PCM is bit-identical to 5.5. */
#include <pspaudio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "vcpe_plat.h"
#include "vcpe_sys.h"
#include "vcpe_pak.h"
#include "vcpe_audio.h"

#ifndef VCPE_AUDIO_FRAMES
#define VCPE_AUDIO_FRAMES 1024   /* output block (1024 = 23 ms); -DVCPE_AUDIO_FRAMES=512 halves the latency */
#endif
#define OUT_FRAMES VCPE_AUDIO_FRAMES
#define RATE 44100
/* Samples per ADPCM block. A mono block is a 4-byte header + 512 bytes of codes; music has one block per channel. */
#define ADPCM_BLOCK 1024
#define SFX_BLOCK_BYTES (4 + ADPCM_BLOCK / 2)
#define MUS_BLOCK_BYTES (2 * (4 + ADPCM_BLOCK / 2))
/* Music ring: 48 stereo blocks = about 1.1 s of music read ahead, which hides slow memory-stick reads. */
#define RING_BLOCKS 48
#define READ_BLOCKS 8           /* blocks per music read */
#define RETRY_US 100000
#define MIXER_PRIORITY 0x12     /* both above the game's main thread (0x20): a busy frame does not starve the sound */
#define READER_PRIORITY 0x14

/* The standard IMA-ADPCM step sizes; index_table says how the step index moves after each code. */
static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

typedef struct { int pred, index; } Adpcm;

/* Decode one 4-bit code: bits 0-2 build the change from the step size, bit 3 is the sign. The prediction is clamped
 * to 16 bits and the step index adapts for the next sample. */
static inline int adpcm_step(Adpcm *s, int code)
{
    int step = step_table[s->index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    if (code & 8) s->pred -= delta; else s->pred += delta;
    if (s->pred > 32767) s->pred = 32767;
    else if (s->pred < -32768) s->pred = -32768;
    s->index += index_table[code];
    if (s->index < 0) s->index = 0;
    else if (s->index > 88) s->index = 88;
    return s->pred;
}

/* A block header: the first sample (16-bit little-endian) and the step index. */
static inline void adpcm_header(Adpcm *s, const uint8_t *b)
{
    s->pred = (int16_t)(b[0] | (b[1] << 8));
    s->index = b[2];
}

/* Code k of a block's code bytes: low nibble first. */
static inline int adpcm_code(const uint8_t *codes, int k) { return (codes[k >> 1] >> ((k & 1) * 4)) & 15; }

/* ---------------------------------------------------------------- SFX voices */
/* A voice: one sound effect playing. pos/frac is the read position in 16.16 fixed point (step = pitch), cur/next are
 * the two samples it interpolates between. active and paused are volatile because the game thread sets them while
 * the mixer thread reads them. */
typedef struct {
    volatile int active;
    int loop;
    const uint8_t *data;
    uint32_t samples;
    uint32_t pos;        /* source sample index of `cur` */
    uint32_t frac;       /* 16.16 */
    uint32_t step;
    int cur, next;
    Adpcm st;
    volatile int paused;
    int gain;            /* 0..256 */
    uint16_t gen;
    uint32_t started;    /* start order, for vcpe_sfx_play stealing */
} Voice;

static uint8_t *sfx_mem;
/* 5.6 sound residency (vcpe_sfx_slab / vcpe_sfx_keep): one fixed block holds only the effects the game asked for;
 * sfx_at[id] is where effect id lives in it (-1: not resident). sfx_mem is then the slab. */
static uint32_t sfx_pak_off, sfx_slab_bytes, sfx_resident_bytes;
static int32_t *sfx_at;
static int sfx_miss_n, sfx_load_n;
static void (*sfx_miss_report)(int id, const char *name);
static int sfx_resident_load(int id);
static const VcpeSound *sfx_tab, *music_tab;
static int nsfx, nvoices;
static Voice voices[VCPE_MAX_VOICES];
static uint32_t start_count;
/* Gains are 0..256 (256 = full). The music gain is also scaled by num/den (vcpe_music_scale) and by the fade. */
static volatile int sfx_vol = 256, mus_vol = 256, mus_paused = 0;
static volatile int mus_num = 1, mus_den = 1;
/* Optional one-pole low-pass on the music ("heard through a wall"): y += (x - y) * k, with k in 1/32768 units
 * (32768: off). */
static volatile int lp_k = 32768;
static int lp_l, lp_r;
static uint64_t mixed_frames;

/* Decode sample i of a voice's effect. Decoding is sequential (each sample depends on the previous), so this must be
 * called with i increasing by one; at the start of each block the decoder state is reset from the block header. */
static int sfx_sample(Voice *v, uint32_t i)
{
    const uint8_t *b = v->data + (i / ADPCM_BLOCK) * SFX_BLOCK_BYTES;
    uint32_t k = i % ADPCM_BLOCK;
    if (k == 0)
        adpcm_header(&v->st, b);
    return adpcm_step(&v->st, adpcm_code(b + 4, (int)k));
}

/* Effects are 22050 Hz, output is 44100 Hz: at pitch 1 the voice advances half a source sample per output sample
 * (32768 = 0.5 in 16.16). */
static uint32_t pitch_step(float pitch) { return (uint32_t)(32768.0f * pitch); }

/* Start effect id on voice vi. The voice is switched off while it is set up, so the mixer thread never sees
 * half-written fields. The returned handle carries a generation counter: a handle to a voice that has since been
 * reused is recognised as stale. */
int vcpe_voice_start(int vi, int id, float pitch, int loop)
{
    if (id < 0 || id >= nsfx || !sfx_mem || vi < 0 || vi >= nvoices)
        return -1;
    if (sfx_at && sfx_at[id] < 0 && sfx_resident_load(id) < 0)
        return -1;
    Voice *v = &voices[vi];
    v->active = 0;
    v->loop = loop;
    v->paused = 0;
    v->gain = 256;
    v->data = sfx_mem + (sfx_at ? (uint32_t)sfx_at[id] : sfx_tab[id].off);
    v->samples = sfx_tab[id].samples;
    v->pos = 0;
    v->frac = 0;
    v->step = pitch_step(pitch);
    v->cur = sfx_sample(v, 0);
    v->next = v->samples > 1 ? sfx_sample(v, 1) : 0;
    v->gen++;
    v->started = ++start_count;
    v->active = 1;
    return (v->gen << 8) | vi;
}

/* Play on a free voice of the range [first, last], or steal the one that started longest ago. */
int vcpe_sfx_play(int id, float pitch, int gain256, int first, int last)
{
    if (first < 0) first = 0;
    if (last >= nvoices) last = nvoices - 1;
    int best = -1;
    for (int vi = first; vi <= last; vi++) {
        if (!voices[vi].active) {
            best = vi;
            break;
        }
        if (best < 0 || voices[vi].started < voices[best].started)
            best = vi;
    }
    if (best < 0)
        return -1;
    int h = vcpe_voice_start(best, id, pitch, 0);
    if (h >= 0)
        voices[best].gain = gain256;
    return h;
}

static Voice *voice(int vi) { return vi >= 0 && vi < nvoices ? &voices[vi] : 0; }

void vcpe_voice_stop(int vi) { Voice *v = voice(vi); if (v) { v->active = 0; v->loop = 0; } }
int vcpe_voice_active(int vi) { Voice *v = voice(vi); return v && v->active; }
void vcpe_voice_pause(int vi, int on) { Voice *v = voice(vi); if (v) v->paused = on; }
void vcpe_voice_pitch(int vi, float pitch) { Voice *v = voice(vi); if (v) v->step = pitch_step(pitch); }
void vcpe_voice_gain(int vi, int gain) { Voice *v = voice(vi); if (v) v->gain = gain; }
int vcpe_voice_count(void) { return nvoices; }

/* Handle back to voice index, or -1 if that voice has been restarted since. */
int vcpe_handle_voice(int h)
{
    if (h < 0)
        return -1;
    int vi = h & 0xFF;
    return vi < nvoices && voices[vi].gen == ((h >> 8) & 0xFFFF) ? vi : -1;
}

void vcpe_voices_stop_all(void)
{
    for (int i = 0; i < nvoices; i++)
        voices[i].active = 0;
}

/* ---------------------------------------------------------------- music */
/* Music streaming state. ring_rd / ring_wr count blocks read by the mixer / written by io_thread (the ring index is
 * the count modulo RING_BLOCKS). gen changes on every track change; the mixer plays only after io_thread acknowledged
 * the new gen (gen_ack), so it never plays blocks of the old track. */
static int mus_fd = -1;
static char mus_path[256];
static uint8_t ring[RING_BLOCKS][MUS_BLOCK_BYTES];
static volatile int ring_rd, ring_wr;
static volatile int want_track = -1, cur_track = -1;
static volatile int gen, gen_ack;
static volatile int fade = 256, fading_out = 0;
static int pending_track = -2;
static int mus_blk_pos;   /* frame inside the current block */
static Adpcm mus_st[2];
static int io_sema = -1;
static volatile int running;

/* Reopen the music PAK after sleep (the old handle is stale) or after a failed read. */
static void music_reopen(int *fd_gen, int64_t *fd_pos)
{
    if (mus_fd >= 0)
        vcpe_file_close(mus_fd);
    *fd_gen = vcpe_resume_gen;
    *fd_pos = -1;
    mus_fd = vcpe_file_open(mus_path);
}

/* The music reader thread. It refills the ring up to READ_BLOCKS blocks per read, sleeps on io_sema when the ring is
 * full, and is woken by the mixer whenever a block was consumed or the track changed. */
static int io_thread(unsigned argc, void *argv)
{
    (void)argc; (void)argv;
    int my_gen = -1, fd_gen = vcpe_resume_gen;
    int64_t fd_pos = -1;   /* where the file is (-1 unknown): sequential reads skip the seek */
    uint32_t off = 0, size = 0, pos = 0;
    while (running) {
        /* Track changed: drop everything in the ring and start reading the new track from its beginning. */
        if (my_gen != gen) {
            my_gen = gen;
            int track = want_track;
            ring_rd = ring_wr = 0;
            pos = 0;
            size = 0;
            if (track >= 0 && mus_fd >= 0) {
                off = music_tab[track].off;
                size = music_tab[track].size / MUS_BLOCK_BYTES * MUS_BLOCK_BYTES;
            }
            gen_ack = my_gen;
        }
        int filled = 0;
        while (running && size && ring_wr - ring_rd < RING_BLOCKS && my_gen == gen) {
            int space = RING_BLOCKS - (ring_wr - ring_rd), contig = RING_BLOCKS - ring_wr % RING_BLOCKS;
            uint32_t left = (size - pos) / MUS_BLOCK_BYTES;
            int n = READ_BLOCKS;
            if (n > space) n = space;
            if (n > contig) n = contig;
            if ((uint32_t)n > left) n = (int)left;
            if (fd_gen != vcpe_resume_gen || mus_fd < 0)
                music_reopen(&fd_gen, &fd_pos);
            /* Read straight into the ring. A short or failed read (memory stick asleep, stale handle) is not queued
             * (the ring would replay old audio): the handle is closed and the read retried 0.1 s later. */
            int want = n * MUS_BLOCK_BYTES, r = -1;
            if (mus_fd >= 0) {
                if (fd_pos != (int64_t)(off + pos))
                    vcpe_file_seek(mus_fd, off + pos);
                r = vcpe_file_read(mus_fd, ring[ring_wr % RING_BLOCKS], (uint32_t)want);
                fd_pos = r == want ? (int64_t)(off + pos) + want : -1;
            }
            if (my_gen != gen)
                break;
            if (r != want) {
                if (mus_fd >= 0)
                    vcpe_file_close(mus_fd);
                mus_fd = -1;
                vcpe_sleep_us(RETRY_US);
                continue;
            }
            ring_wr += n;
            pos += (uint32_t)want;
            if (pos >= size)
                pos = 0;   /* tracks loop */
            filled = 1;
        }
        if (!filled)
            vcpe_sema_wait_cb(io_sema);
    }
    return 0;
}

/* Decode n music frames into lr (interleaved stereo), or silence when paused, while a track change is pending or when
 * the ring ran dry. Left and right are separate ADPCM streams in the same block; runs stop at block ends, where the
 * block is handed back to the reader and the checks are made again. */
static void music_run(int *lr, int n)
{
    while (n > 0) {
        if (gen_ack != gen || ring_rd >= ring_wr || mus_paused) {
            memset(lr, 0, (size_t)n * 2 * sizeof *lr);
            return;
        }
        const uint8_t *b = ring[ring_rd % RING_BLOCKS];
        int k = mus_blk_pos, run = ADPCM_BLOCK - k;
        if (run > n)
            run = n;
        if (k == 0) {
            adpcm_header(&mus_st[0], b);
            adpcm_header(&mus_st[1], b + 4);
        }
        const uint8_t *cl = b + 8, *cr = cl + ADPCM_BLOCK / 2;
        Adpcm l = mus_st[0], r = mus_st[1];
        for (int end = k + run; k < end; k++) {
            *lr++ = adpcm_step(&l, adpcm_code(cl, k));
            *lr++ = adpcm_step(&r, adpcm_code(cr, k));
        }
        mus_st[0] = l;
        mus_st[1] = r;
        n -= run;
        mus_blk_pos = k;
        if (k == ADPCM_BLOCK) {
            mus_blk_pos = 0;
            ring_rd++;
            vcpe_sema_signal(io_sema);
        }
    }
}

/* Switch tracks: bump the generation and wake io_thread to refill the ring. */
static void start_track(int t)
{
    cur_track = t;
    want_track = t;
    mus_blk_pos = 0;
    gen++;
    vcpe_sema_signal(io_sema);
}

/* Change the music. With a track already playing, it is faded out first (see mix_thread) and the new one starts
 * after. */
void vcpe_music_play(int track)
{
    if (track == cur_track && !fading_out)
        return;
    if (cur_track < 0) {
        fade = 256;
        start_track(track);
        return;
    }
    pending_track = track;
    fading_out = 1;
}

int vcpe_music_current(void) { return fading_out ? pending_track : cur_track; }

void vcpe_music_stop_now(void)
{
    fading_out = 0;
    start_track(-1);
}

void vcpe_sfx_gain(int gain256) { sfx_vol = gain256; }
void vcpe_music_gain(int gain256) { mus_vol = gain256; }
void vcpe_music_scale(int num, int den) { mus_den = den > 0 ? den : 1; mus_num = num; }

/* Cut-off frequency to the one-pole coefficient: a = 1 - e^(-2*pi*f/44100). 0 or 20 kHz and above switch it off. */
void vcpe_music_lowpass(float hz)
{
    if (hz <= 0 || hz >= 20000) {
        lp_k = 32768;
        return;
    }
    float a = 1.0f - expf(-6.2831853f * hz / (float)RATE);
    int k = (int)(a * 32767);
    lp_k = k < 1 ? 1 : k;
}

void vcpe_music_pause(int on) { mus_paused = on; }
uint32_t vcpe_audio_ms(void) { return (uint32_t)(mixed_frames * 1000 / RATE); }

/* ---------------------------------------------------------------- output */
/* Two output buffers, alternated: one is queued to the hardware while the other is mixed. */
static short __attribute__((aligned(64))) outbuf[2][OUT_FRAMES * 2];
static int audio_ch = -1;
static int mix_acc[OUT_FRAMES];         /* effects, mono, before the effect gain */
static int mus_lr[OUT_FRAMES * 2];      /* music, stereo, before the low-pass and gain */
static int hook_lr[OUT_FRAMES * 2];     /* vcpe_audio_mix_hook's stereo sources */
/* extra stereo sources (vcpe_stream) mixed after the gains; 0 = none */
void (*vcpe_audio_mix_hook)(int *lr, int frames);

static inline short clip16(int s) { return s > 32767 ? 32767 : (s < -32768 ? -32768 : s); }

/* Mix one voice into the accumulator for a whole block: linear interpolation between cur and next by the fraction,
 * times the gain. Whenever the fraction passes 1.0, advance one source sample; at the end, loop or stop. */
static void mix_voice(Voice *v, int *acc, int n)
{
    int cur = v->cur, next = v->next, gain = v->gain, ended = 0;
    uint32_t frac = v->frac, step = v->step, pos = v->pos, samples = v->samples;
    for (int i = 0; i < n && !ended; i++) {
        acc[i] += ((cur + (((next - cur) * (int)(frac >> 4)) >> 12)) * gain) >> 8;
        frac += step;
        while (frac >= 65536) {
            frac -= 65536;
            pos++;
            cur = next;
            if (pos + 1 < samples) {
                next = sfx_sample(v, pos + 1);
            } else if (pos >= samples) {
                if (v->loop) {
                    pos = 0;
                    cur = sfx_sample(v, 0);
                    next = sfx_sample(v, 1);
                    continue;
                }
                v->active = 0;
                ended = 1;
                break;
            } else {
                next = 0;
            }
        }
    }
    v->cur = cur;
    v->next = next;
    v->frac = frac;
    v->pos = pos;
}

/* Mix one output block. */
static void mix_block(short *out)
{
    /* Song change in progress: lower the fade every block (24/256 per 23 ms, about 0.25 s to silence), then start the
     * waiting track at full volume. */
    if (fading_out) {
        fade -= 24;
        if (fade <= 0) {
            fade = 256;
            fading_out = 0;
            start_track(pending_track);
            /* The new track's first frames must come from the reader's ring, not from the silence it gives until it
             * acknowledges the change: wait for that here, so the output never depends on thread timing. */
            while (gen_ack != gen && running)
                vcpe_sleep_us(200);
        }
    }
    int mg = (((mus_vol * fade) >> 8) * mus_num) / mus_den, sg = sfx_vol;
    mixed_frames += OUT_FRAMES;
    /* Effects first: every active voice into the integer accumulator (no clipping yet, so loud overlaps sum). */
    memset(mix_acc, 0, sizeof mix_acc);
    for (int vi = 0; vi < nvoices; vi++) {
        Voice *v = &voices[vi];
        if (v->active && !v->paused)
            mix_voice(v, mix_acc, OUT_FRAMES);
    }
    void (*hook)(int *, int) = vcpe_audio_mix_hook;
    if (hook) {
        memset(hook_lr, 0, sizeof hook_lr);
        hook(hook_lr, OUT_FRAMES);
    }
    music_run(mus_lr, OUT_FRAMES);
    /* Then per frame: music (with the low-pass) and gains, effects added to both channels, clipped to 16 bits. */
    int k = lp_k, lpl = lp_l, lpr = lp_r;
    for (int i = 0; i < OUT_FRAMES; i++) {
        int l = mus_lr[i * 2], r = mus_lr[i * 2 + 1];
        if (k < 32768) {
            lpl += ((l - lpl) * k) >> 15;
            lpr += ((r - lpr) * k) >> 15;
            l = lpl;
            r = lpr;
        }
        l = (l * mg) >> 8;
        r = (r * mg) >> 8;
        int s = (mix_acc[i] * sg) >> 8;
        if (hook) {
            l += hook_lr[i * 2];
            r += hook_lr[i * 2 + 1];
        }
        out[i * 2] = clip16(l + s);
        out[i * 2 + 1] = clip16(r + s);
    }
    lp_l = lpl;
    lp_r = lpr;
}

/* The mixer thread: one block per iteration. Queuing the block blocks until the hardware can take it, which is what
 * paces the thread to real time. */
static int mix_thread(unsigned argc, void *argv)
{
    (void)argc; (void)argv;
    int which = 0;
    while (running) {
        short *out = outbuf[which];
        which ^= 1;
        mix_block(out);
        sceAudioOutputPannedBlocking(audio_ch, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, out);
    }
    return 0;
}

/* Load the effects (whole, compressed), open the music PAK, reserve an output channel and start the two threads. */
int vcpe_audio_init(const char *music_pak, int voices_n, uint32_t sfx_off, uint32_t sfx_size,
                    const VcpeSound *sfx, int sfx_n, const VcpeSound *music)
{
    nvoices = voices_n > VCPE_MAX_VOICES ? VCPE_MAX_VOICES : voices_n;
    sfx_tab = sfx;
    nsfx = sfx_n;
    music_tab = music;
    sfx_pak_off = sfx_off;
    int st;
    if (sfx_size) {   /* sfx_size 0: residency mode, the game calls vcpe_sfx_slab + vcpe_sfx_keep */
        st = vcpe_step_begin("Loading sound effects: %d, %u KB", sfx_n, (unsigned)(sfx_size / 1024));
        sfx_mem = vcpe_pak_read_alloc(sfx_off, sfx_size);
        vcpe_step_end(st, sfx_mem != 0);
    }
    snprintf(mus_path, sizeof mus_path, "%s", music_pak);
    const char *mname = strrchr(music_pak, '/');
    st = vcpe_step_begin("Opening music %s", mname ? mname + 1 : music_pak);
    mus_fd = vcpe_file_open(music_pak);
    vcpe_step_end(st, mus_fd >= 0);
    st = vcpe_step_begin("Starting audio output (%d voices)", nvoices);
    audio_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    vcpe_step_end(st, audio_ch >= 0);
    if (audio_ch < 0)
        return audio_ch;
    running = 1;
    io_sema = vcpe_sema_new("mus_io", 0, 64);
    vcpe_thread_start("mus_io", io_thread, READER_PRIORITY, 0x4000, 0, 0, 0);
    vcpe_thread_start("mixer", mix_thread, MIXER_PRIORITY, 0x8000, 0, 0, 0);
    return 0;
}

/* Stop both threads (wake the reader so it sees running = 0), give them time to finish, release the channel. */
void vcpe_audio_shutdown(void)
{
    running = 0;
    if (io_sema >= 0)
        vcpe_sema_signal(io_sema);
    vcpe_sleep_us(50000);
    if (audio_ch >= 0)
        sceAudioChRelease(audio_ch);
    if (mus_fd >= 0)
        vcpe_file_close(mus_fd);
}

/* ---------------------------------------------------------------- 5.6 sound residency
 * The usual setup reads every effect into RAM at boot. A game whose levels use different sounds can instead keep one
 * fixed slab (no heap fragmentation, the size of its biggest set) and swap the set behind the loading screen.
 * The slab is laid out as a list of resident effects sorted by address; a new effect goes into the first gap that
 * fits. Effects that a voice is still playing are never moved or overwritten. */
#define SFX_ALIGN 16u
static uint32_t al(uint32_t n) { return (n + SFX_ALIGN - 1) & ~(SFX_ALIGN - 1); }

int vcpe_sfx_slab(uint32_t bytes)
{
    if (sfx_at || sfx_mem || !nsfx)
        return -1;   /* already set up, or vcpe_audio_init read every effect (pass sfx_size 0) */
    sfx_at = malloc(sizeof *sfx_at * (size_t)nsfx);
    sfx_mem = memalign(64, bytes = al(bytes));
    if (!sfx_at || !sfx_mem) {
        free(sfx_at); free(sfx_mem);
        sfx_at = 0; sfx_mem = 0;
        return -1;
    }
    for (int i = 0; i < nsfx; i++) sfx_at[i] = -1;
    sfx_slab_bytes = bytes;
    sfx_resident_bytes = 0;
    return 0;
}

/* Resident effects in address order (insertion sort: a few hundred at most, at load time only). */
static int by_addr(int *ids)
{
    int n = 0;
    for (int i = 0; i < nsfx; i++)
        if (sfx_at[i] >= 0) {
            int j = n++;
            while (j > 0 && sfx_at[ids[j - 1]] > sfx_at[i]) { ids[j] = ids[j - 1]; j--; }
            ids[j] = i;
        }
    return n;
}

/* first gap of `need` bytes, or -1 */
static int32_t find_gap(uint32_t need)
{
    int *ids = malloc(sizeof *ids * (size_t)nsfx);
    if (!ids) return -1;
    int n = by_addr(ids);
    uint32_t at = 0;
    int32_t found = -1;
    for (int k = 0; k <= n; k++) {
        uint32_t end = k < n ? (uint32_t)sfx_at[ids[k]] : sfx_slab_bytes;
        if (end >= at && end - at >= need) { found = (int32_t)at; break; }
        if (k < n) at = (uint32_t)sfx_at[ids[k]] + al(sfx_tab[ids[k]].size);
    }
    free(ids);
    return found;
}

static int place(int id)
{
    int32_t at = find_gap(al(sfx_tab[id].size));
    if (at < 0) return -1;
    if (vcpe_pak_read(sfx_pak_off + sfx_tab[id].off, sfx_mem + at, sfx_tab[id].size) < 0) return -1;
    sfx_at[id] = at;
    sfx_resident_bytes += al(sfx_tab[id].size);
    sfx_load_n++;
    return 0;
}

static void drop(int id)
{
    if (sfx_at[id] < 0) return;
    sfx_at[id] = -1;
    sfx_resident_bytes -= al(sfx_tab[id].size);
}

static int playing(int id)
{
    if (sfx_at[id] < 0) return 0;
    const uint8_t *p = sfx_mem + sfx_at[id];
    for (int vi = 0; vi < nvoices; vi++)
        if (voices[vi].active && voices[vi].data == p) return 1;
    return 0;
}

/* Voices just switched off may still be inside the mixer's current block: wait for the mixer to start a new one
 * (bounded, so a stalled or host-paced mixer can't hang a load) before their bytes are overwritten. */
static void mixer_settle(void)
{
    volatile uint64_t *mf = &mixed_frames;
    uint64_t m0 = *mf;
    uint64_t t0 = vcpe_time_us();
    while (running && *mf < m0 + 2 * OUT_FRAMES && vcpe_time_us() - t0 < 100000)
        vcpe_sleep_us(1000);
}

/* A sound started that the game's set did not have: read it now (a short hitch), report it once per id. */
static int sfx_resident_load(int id)
{
    sfx_miss_n++;
    if (sfx_miss_report) sfx_miss_report(id, sfx_tab[id].name);
    return place(id);
}

int vcpe_sfx_keep(const uint8_t *want)
{
    if (!sfx_at)
        return -1;
    int st = vcpe_step_begin("Sound effects for this scene");
    /* 1. drop what the next scene does not use, unless a voice is still playing it */
    for (int i = 0; i < nsfx; i++)
        if (!(want[i >> 3] >> (i & 7) & 1) && !playing(i))
            drop(i);
    /* 2. add the missing ones, largest first (fewer gaps) */
    int *ids = malloc(sizeof *ids * (size_t)nsfx), n = 0, failed = 0;
    if (!ids) { vcpe_step_end(st, 0); return -1; }
    for (int i = 0; i < nsfx; i++)
        if ((want[i >> 3] >> (i & 7) & 1) && sfx_at[i] < 0) {
            int j = n++;
            while (j > 0 && sfx_tab[ids[j - 1]].size < sfx_tab[i].size) { ids[j] = ids[j - 1]; j--; }
            ids[j] = i;
        }
    int k = 0;
    for (; k < n; k++)
        if (place(ids[k]) < 0) break;
    if (k < n) {
        /* 3. out of gaps: stop every voice (bytes are about to move), then pack the slab from the start (memmove in
         * address order never overwrites an effect before it has moved) and place the rest after it */
        for (int vi = 0; vi < nvoices; vi++) voices[vi].active = 0;   /* moved bytes: no voice may point into them */
        mixer_settle();
        for (int i = 0; i < nsfx; i++)
            if (!(want[i >> 3] >> (i & 7) & 1)) drop(i);
        int *ord = malloc(sizeof *ord * (size_t)nsfx);
        if (ord) {
            int m = by_addr(ord);
            uint32_t at = 0;
            for (int q = 0; q < m; q++) {
                int i = ord[q];
                if ((uint32_t)sfx_at[i] != at) memmove(sfx_mem + at, sfx_mem + sfx_at[i], sfx_tab[i].size);
                sfx_at[i] = (int32_t)at;
                at += al(sfx_tab[i].size);
            }
            free(ord);
        }
        for (; k < n; k++)
            if (place(ids[k]) < 0) failed++;
    }
    free(ids);
    vcpe_step_end(st, failed == 0);
    return failed ? -1 : 0;
}

uint32_t vcpe_sfx_resident_kb(void) { return sfx_resident_bytes / 1024; }
uint32_t vcpe_sfx_slab_kb(void) { return sfx_slab_bytes / 1024; }
int vcpe_sfx_misses(void) { return sfx_miss_n; }
int vcpe_sfx_resident(int id) { return id >= 0 && id < nsfx && (!sfx_at ? sfx_mem != 0 : sfx_at[id] >= 0); }
void vcpe_sfx_miss_hook(void (*report)(int id, const char *name)) { sfx_miss_report = report; }
