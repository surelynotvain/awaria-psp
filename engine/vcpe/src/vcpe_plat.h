/* VCPE platform layer: the kernel services engine modules use (clock, sleeping, CPU cache, files, threads,
 * semaphores), in one place instead of scattered PSP SDK calls.
 *
 * On the PSP every function is a static inline wrapper around one SDK call, so using it costs nothing and links
 * nothing new (games keep their VCPE_MODULES lists). Anywhere else (host tests, PC tools) the same functions come
 * from src/vcpe_plat_host.c (POSIX: clock_gettime, open/read, pthreads, semaphores), so modules written against this
 * header build and run on a PC unchanged. Porting VCPE away from the PSP SDK means replacing the PSP half below.
 *
 * Rendering is not here: the GE command calls stay inside the drawing modules (vcpe_gfx and friends), which are the
 * graphics backend. */
#ifndef VCPE_PLAT_H
#define VCPE_PLAT_H
#include <stdint.h>

/* Thread body: argc/argv are the bytes given to vcpe_thread_start, copied to the new thread's stack. */
typedef int (*VcpeThreadFn)(unsigned argc, void *argv);
enum { VCPE_THREAD_VFPU = 1 };   /* the thread uses the VFPU (its registers are saved on switches) */

/* VCPE_PLAT_PSP_SDK: use the PSP half on another machine too, against stand-ins for the SDK (tests/hostrun). */
#if defined(__psp__) || defined(VCPE_PLAT_PSP_SDK)
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>

/* ---------------------------------------------------------------- time */
static inline uint64_t vcpe_time_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }
static inline uint32_t vcpe_time_us32(void) { return sceKernelGetSystemTimeLow(); }
static inline void vcpe_sleep_us(uint32_t us) { sceKernelDelayThread(us); }

/* ---------------------------------------------------------------- CPU data cache
 * The GE and DMA read RAM directly: anything the CPU wrote for them (textures, palettes, vertices built outside the
 * display list) has to be written back from the data cache first. */
static inline void vcpe_cache_writeback(const void *p, uint32_t n) { sceKernelDcacheWritebackRange(p, n); }
static inline void vcpe_cache_writeback_all(void) { sceKernelDcacheWritebackAll(); }

/* ---------------------------------------------------------------- files (fd < 0: failure) */
static inline int vcpe_file_open(const char *path) { return sceIoOpen(path, PSP_O_RDONLY, 0); }
static inline int vcpe_file_read(int fd, void *dst, uint32_t n) { return sceIoRead(fd, dst, n); }
static inline int vcpe_file_seek(int fd, uint32_t off) { return sceIoLseek32(fd, (int)off, PSP_SEEK_SET); }
static inline int vcpe_file_size(int fd) { return sceIoLseek32(fd, 0, PSP_SEEK_END); }   /* moves the position */
static inline void vcpe_file_close(int fd) { sceIoClose(fd); }
/* writing: a new (emptied) file, or appending to one; fd < 0: failure */
static inline int vcpe_file_create(const char *path, int append)
{
    return sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | (append ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
}
static inline int vcpe_file_write(int fd, const void *src, uint32_t n) { return sceIoWrite(fd, src, n); }

/* ---------------------------------------------------------------- threads and semaphores
 * Priorities follow the PSP: smaller runs first; a game's main thread is 0x20. */
static inline int vcpe_thread_start(const char *name, VcpeThreadFn fn, int priority, int stack, int flags,
                                    unsigned argc, void *argv)
{
    int th = sceKernelCreateThread(name, (SceKernelThreadEntry)fn, priority, stack,
                                   PSP_THREAD_ATTR_USER | (flags & VCPE_THREAD_VFPU ? PSP_THREAD_ATTR_VFPU : 0), 0);
    if (th >= 0 && sceKernelStartThread(th, argc, argv) < 0) {
        sceKernelDeleteThread(th);
        return -1;
    }
    return th;
}
static inline void vcpe_thread_kill(int th) { sceKernelTerminateDeleteThread(th); }
static inline void vcpe_thread_exit(void) { sceKernelExitDeleteThread(0); }
static inline int vcpe_sema_new(const char *name, int initial, int max) { return sceKernelCreateSema(name, 0, initial, max, 0); }
static inline void vcpe_sema_wait(int s) { sceKernelWaitSema(s, 1, 0); }
/* also runs pending callbacks of this thread while it waits (only matters for threads that registered some) */
static inline void vcpe_sema_wait_cb(int s) { sceKernelWaitSemaCB(s, 1, 0); }
static inline void vcpe_sema_signal(int s) { sceKernelSignalSema(s, 1); }
static inline void vcpe_sema_delete(int s) { sceKernelDeleteSema(s); }

#else   /* host: src/vcpe_plat_host.c; the clock is inline where POSIX is visible */
#include <time.h>
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L && !defined(VCPE_PLAT_HOST_IMPL)
static inline uint64_t vcpe_time_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
static inline uint32_t vcpe_time_us32(void) { return (uint32_t)vcpe_time_us(); }
static inline void vcpe_sleep_us(uint32_t us)
{
    struct timespec t = {(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000};
    nanosleep(&t, 0);
}
#else   /* strict ISO C builds: the same functions, from vcpe_plat_host.c */
uint64_t vcpe_time_us(void);
uint32_t vcpe_time_us32(void);
void vcpe_sleep_us(uint32_t us);
#endif
static inline void vcpe_cache_writeback(const void *p, uint32_t n) { (void)p; (void)n; }
static inline void vcpe_cache_writeback_all(void) {}
int vcpe_file_open(const char *path);
int vcpe_file_read(int fd, void *dst, uint32_t n);
int vcpe_file_seek(int fd, uint32_t off);
int vcpe_file_size(int fd);
void vcpe_file_close(int fd);
int vcpe_file_create(const char *path, int append);
int vcpe_file_write(int fd, const void *src, uint32_t n);
int vcpe_thread_start(const char *name, VcpeThreadFn fn, int priority, int stack, int flags, unsigned argc,
                      void *argv);
void vcpe_thread_kill(int th);
void vcpe_thread_exit(void);
int vcpe_sema_new(const char *name, int initial, int max);
void vcpe_sema_wait(int s);
void vcpe_sema_wait_cb(int s);
void vcpe_sema_signal(int s);
void vcpe_sema_delete(int s);

#endif
#endif
