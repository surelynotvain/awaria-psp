/* VCPE data PAK: one big read-only file, read by offset; survives sleep/resume and slow memory sticks. */

/* HOW THIS FILE WORKS
 *
 * A game keeps all its data in one big file (the PAK) and reads pieces by offset. Two PSP problems are handled here:
 * - After the console sleeps and wakes, open file handles go stale and reads fail. vcpe_sys counts resumes in
 *   vcpe_resume_gen; when it changed, the file is reopened before reading.
 * - Memory sticks can be slow to wake or fail a read. A failed read is retried up to 50 times, 0.1 s apart (5 s),
 *   reopening the file each time. The file position is tracked, so consecutive reads (the common case while loading)
 *   skip the seek system call. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "vcpe_plat.h"
#include "vcpe_sys.h"
#include "vcpe_pak.h"
#include "vcpe_observe.h"

#define RETRIES 50
#define RETRY_US 100000
#define CHUNK (256u * 1024)   /* read size while a loading screen wants progress */

/* Set by the loading screen; called after every chunk so it can redraw. */
void (*vcpe_pak_progress)(uint32_t bytes);
void vcpe_load_assert_observe(uint32_t bytes) __attribute__((weak));   /* vcpe_load's read-contract monitor */

static int pak_fd = -1;
static char pak_path[300];
static int pak_gen;
static int64_t pak_pos = -1;   /* -1 = unknown (after opening, a failed read or a seek elsewhere): the next read seeks */

/* Close (if open) and open the PAK again: first open, after sleep, or after a failed read. */
static void pak_reopen(void)
{
    if (pak_fd >= 0)
        vcpe_file_close(pak_fd);
    pak_gen = vcpe_resume_gen;
    pak_fd = vcpe_file_open(pak_path);
    pak_pos = -1;
}

int vcpe_pak_open(const char *path)
{
    snprintf(pak_path, sizeof pak_path, "%s", path);
    pak_reopen();   /* also closes a PAK opened before */
    return pak_fd;
}

/* Open the PAK and compare its header with what the EBOOT was built for (magic, total size, build stamp). Players
 * copy files by hand: a missing, truncated or mismatched PAK gets a clear message instead of a crash later. */
void vcpe_pak_open_checked(const char *file, const char *title, const char *magic, uint32_t size, uint32_t stamp)
{
    char p[300], msg[600];
    vcpe_path(p, sizeof p, file);
    if (vcpe_pak_open(p) < 0) {
        snprintf(msg, sizeof msg, "%s is missing.\n\nCopy %s next to EBOOT.PBP (all files of the same build).", file, file);
        vcpe_fatal(title, msg);
    }
    struct { char magic[4]; uint32_t size, stamp; } h;
    int bad = vcpe_pak_read(0, &h, sizeof h) || (magic && memcmp(h.magic, magic, 4));
    if (!bad && size) {
        int end = vcpe_file_size(pak_fd);
        pak_pos = -1;
        bad = h.size != size || end < 0 || (uint32_t)end < size;
    }
    if (!bad && stamp)
        bad = h.stamp != stamp;
    if (bad) {
        snprintf(msg, sizeof msg, "%s does not match this EBOOT.PBP (an incomplete copy, or a file from another "
                 "version).\n\nCopy EBOOT.PBP and all the PAK files again, all from the same build.", file);
        vcpe_fatal(title, msg);
    }
}

/* the PAK's path, for modules that open their own handle on it (vcpe_pack's background loader) */
const char *vcpe_pak_path(void) { return pak_path; }

void vcpe_pak_close(void)
{
    if (pak_fd >= 0)
        vcpe_file_close(pak_fd);
    pak_fd = -1;
    pak_pos = -1;
}

/* One attempt: position the file (unless it already is there) and read everything; reports progress per chunk.
 * Returns the bytes read; a short count leaves the position unknown. */
static uint32_t read_once(uint32_t off, uint8_t *mem, uint32_t size)
{
    if (pak_pos != (int64_t)off) {
        if (vcpe_file_seek(pak_fd, off) != (int)off) {
            pak_pos = -1;
            return 0;
        }
        pak_pos = off;
    }
    uint32_t got = 0;
    void (*progress)(uint32_t) = vcpe_pak_progress;
    while (got < size) {
        uint32_t n = size - got;
        if (progress && n > CHUNK)
            n = CHUNK;   /* a loading screen redraws between chunks */
        int r = vcpe_file_read(pak_fd, mem + got, n);
        if (r <= 0) {
            pak_pos = -1;
            break;
        }
        got += (uint32_t)r;
        pak_pos += r;
        if (progress)
            progress((uint32_t)r);
    }
    return got;
}

/* Read size bytes at offset off into dst. 0 on success, -1 after every retry failed. */
int vcpe_pak_read(uint32_t off, void *dst, uint32_t size)
{
    if (vcpe_load_assert_observe)
        vcpe_load_assert_observe(size);
    uint32_t observed = vcpe_observe_begin();
    uint32_t got = 0;
    for (int attempt = 0; attempt < RETRIES && got != size; attempt++) {
        if (attempt) {
            /* a failed read (stale handle after sleep, stick still waking up): wait, reopen, retry */
            vcpe_sleep_us(RETRY_US);
            pak_reopen();
        } else if (pak_gen != vcpe_resume_gen || pak_fd < 0) {
            pak_reopen();
        }
        if (pak_fd >= 0)
            got = read_once(off, dst, size);
    }
    vcpe_observe_end(VCPE_COST_IO, observed);
    return got == size ? 0 : -1;
}

/* Allocate and read in one go. The block is 64-byte aligned (GE and DMA friendly) and written back from the CPU
 * cache, so the GE can use it as texture data right away. */
void *vcpe_pak_read_alloc(uint32_t off, uint32_t size)
{
    const char *name = strrchr(pak_path, '/');
    int st = vcpe_step_begin("Reading %s @%08x: %u KB", name ? name + 1 : pak_path, (unsigned)off,
                             (unsigned)((size + 1023) / 1024));
    uint8_t *mem = memalign(64, size ? size : 64);
    if (!mem) {
        vcpe_observe_fail("PAK", size);
        vcpe_step_end(st, 0);
        return 0;
    }
    if (vcpe_pak_read(off, mem, size) < 0) {
        free(mem);
        vcpe_step_end(st, 0);
        return 0;
    }
    vcpe_cache_writeback(mem, size);
    vcpe_step_end(st, 1);
    return mem;
}
