/* VCPE log file (see vcpe_log.h). */

/* A text log next to the EBOOT, for test builds and bug reports. By default every line is flushed to the memory stick
 * at once, so the log survives a crash; vcpe_log_buffered(1) skips that (much faster when logging a lot, but the last
 * lines may be lost in a crash).
 *
 * 5.6: the file gets a 16 KB stdio buffer, so buffered logging reaches the memory stick in a few large writes instead
 * of one per 1 KB (newlib's default), and vcpe_fatal's lines share the same writer. */
#include <stdarg.h>
#include <stdio.h>
#include "vcpe_sys.h"
#include "vcpe_log.h"

#define LOG_BUFFER 16384

static FILE *logf_;
static int buffered;
static char buffer[LOG_BUFFER];

int vcpe_log_open(const char *file)
{
    char p[300];
    vcpe_path(p, sizeof p, file);
    vcpe_log_close();
    logf_ = fopen(p, "w");
    if (!logf_)
        return -1;
    setvbuf(logf_, buffer, _IOFBF, sizeof buffer);
    return 0;
}

/* End a line: unbuffered mode (or force) pushes it to the memory stick now. */
static void end_line(int force)
{
    fputc('\n', logf_);
    if (force || !buffered)
        fflush(logf_);
}

void vcpe_log(const char *fmt, ...)
{
    if (!logf_)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf_, fmt, ap);
    va_end(ap);
    end_line(0);
}

/* vcpe_fatal writes its message here before it takes the screen (weak link in vcpe_sys): always flushed. */
void vcpe_log_line(const char *s)
{
    if (!logf_)
        return;
    fputs(s, logf_);
    end_line(1);
}

void vcpe_log_buffered(int on)
{
    buffered = on;
    if (!on)
        vcpe_log_flush();
}

void vcpe_log_flush(void)
{
    if (logf_)
        fflush(logf_);
}

int vcpe_log_is_open(void) { return logf_ != 0; }

void vcpe_log_close(void)
{
    if (logf_)
        fclose(logf_);
    logf_ = 0;
}
