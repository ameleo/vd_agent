/*
 * spice-cups-backend.c — CUPS backend that forwards print jobs over the
 * DaaS SPICE virtio-serial port to the SPICE client.
 *
 * CUPS calling convention:
 *   no args       → device discovery: print one "direct spice://…" line
 *   5 or 6 args   → process job: job-id user title copies options [file]
 *
 * The document data is read from <file> or stdin when no file is given.
 * CUPS sets the CONTENT_TYPE environment variable with the MIME type of
 * the data being sent (e.g. "application/pdf").
 *
 * Wire protocol: see print-protocol.h
 *
 * Installation:
 *   Install to $(cups_serverbin)/backend/spice  (mode 0700, owned by root)
 *   CUPS only runs backends that are root-owned and not world-writable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <endian.h>

#include "print-protocol.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

#define VIRTIO_PORT  "/dev/virtio-ports/org.daas.printer.0"
#define CHUNK_SIZE   (64u * 1024u)

/*
 * CUPS backend exit codes (defined in <cups/backend.h>, but we avoid the
 * dependency to keep the binary self-contained).
 */
#define CUPS_BACKEND_OK            0
#define CUPS_BACKEND_FAILED        1
#define CUPS_BACKEND_AUTH_REQUIRED 2
#define CUPS_BACKEND_HOLD          3
#define CUPS_BACKEND_STOP          4   /* stop the queue – hardware missing */
#define CUPS_BACKEND_CANCEL        5

/* ── Module state ─────────────────────────────────────────────────────────── */

static int      port_fd         = -1;
static uint32_t current_job_id  = 0;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_frame(uint8_t type, const void *payload, uint32_t size)
{
    struct daas_print_frame hdr;
    hdr.magic  = htole32(DAAS_PRINT_MAGIC);
    hdr.job_id = htole32(current_job_id);
    hdr.type   = type;
    hdr.size   = htole32(size);

    if (write_all(port_fd, &hdr, sizeof(hdr)) < 0)
        return -1;
    if (size && write_all(port_fd, payload, size) < 0)
        return -1;
    return 0;
}

/*
 * Map CUPS CONTENT_TYPE to a DAAS_PRINT_FMT_* constant.
 * If unrecognised, fall back to raw so the client can still try.
 */
static uint8_t detect_format(void)
{
    const char *ct = getenv("CONTENT_TYPE");
    if (!ct)
        return DAAS_PRINT_FMT_RAW;
    if (strstr(ct, "pdf"))
        return DAAS_PRINT_FMT_PDF;
    if (strstr(ct, "postscript") || strstr(ct, "ps"))
        return DAAS_PRINT_FMT_PS;
    if (strstr(ct, "pwg-raster") || strstr(ct, "urf"))
        return DAAS_PRINT_FMT_PWG_RASTER;
    return DAAS_PRINT_FMT_RAW;
}

/*
 * Build and send the JOB_START frame.
 * title, user and options may be NULL (treated as empty).
 */
static int send_job_start(const char *title, const char *user,
                          const char *options)
{
    uint8_t  fmt         = detect_format();
    uint16_t title_len   = title   ? (uint16_t)strlen(title)   : 0;
    uint16_t user_len    = user    ? (uint16_t)strlen(user)    : 0;
    uint16_t options_len = options ? (uint16_t)strlen(options) : 0;

    /* 1 (format) + 3×2 (length fields) + strings */
    uint32_t payload_size = 1u + 2u + 2u + 2u
                            + title_len + user_len + options_len;

    uint8_t *payload = malloc(payload_size);
    if (!payload) return -1;

    uint8_t  *p  = payload;
    uint16_t  tl = htole16(title_len);
    uint16_t  ul = htole16(user_len);
    uint16_t  ol = htole16(options_len);

    *p++ = fmt;
    memcpy(p, &tl, 2); p += 2;
    memcpy(p, &ul, 2); p += 2;
    memcpy(p, &ol, 2); p += 2;

    if (title_len)   { memcpy(p, title,   title_len);   p += title_len;   }
    if (user_len)    { memcpy(p, user,    user_len);    p += user_len;    }
    if (options_len) { memcpy(p, options, options_len);                   }

    int ret = send_frame(DAAS_PRINT_JOB_START, payload, payload_size);
    free(payload);
    return ret;
}

/* Read all data from in_fd and forward it as JOB_DATA frames. */
static int forward_job_data(int in_fd)
{
    static uint8_t buf[CHUNK_SIZE];
    ssize_t n;

    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        if (send_frame(DAAS_PRINT_JOB_DATA, buf, (uint32_t)n) < 0) {
            fprintf(stderr, "ERROR: write to %s failed: %s\n",
                    VIRTIO_PORT, strerror(errno));
            return -1;
        }
    }
    if (n < 0) {
        fprintf(stderr, "ERROR: reading job data: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void abort_job(void)
{
    if (port_fd >= 0)
        send_frame(DAAS_PRINT_JOB_ABORT, NULL, 0);
}

/* Signal handler: cancel the in-flight job cleanly before exiting. */
static void handle_sigterm(int sig)
{
    (void)sig;
    abort_job();
    _exit(CUPS_BACKEND_CANCEL);
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /*
     * Device discovery — CUPS calls the backend with no arguments to
     * enumerate available devices.  One device per line:
     *   class uri "make-and-model" "info" ["device-id"]
     */
    if (argc == 1) {
        puts("direct spice:/// \"DaaS SPICE Printer\" \"DaaS SPICE virtual printer\"");
        return CUPS_BACKEND_OK;
    }

    if (argc < 6 || argc > 7) {
        fprintf(stderr,
                "Usage: spice job-id user title copies options [file]\n");
        return CUPS_BACKEND_FAILED;
    }

    current_job_id    = (uint32_t)atoi(argv[1]);
    const char *user  = argv[2];
    const char *title = argv[3];
    /* argv[4] = copies (handled by filter chain, we ignore it) */
    const char *options  = argv[5];
    const char *filename = (argc == 7) ? argv[6] : NULL;

    signal(SIGTERM, handle_sigterm);
    signal(SIGPIPE, SIG_IGN);

    /* Open document source */
    int in_fd = STDIN_FILENO;
    if (filename) {
        in_fd = open(filename, O_RDONLY);
        if (in_fd < 0) {
            fprintf(stderr, "ERROR: cannot open job file %s: %s\n",
                    filename, strerror(errno));
            return CUPS_BACKEND_FAILED;
        }
    }

    /* Open virtio port (write-only; the client sends JOB_STATUS asynchronously
     * but we do not block waiting for it in this initial implementation). */
    port_fd = open(VIRTIO_PORT, O_WRONLY);
    if (port_fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n",
                VIRTIO_PORT, strerror(errno));
        fprintf(stderr, "INFO: Is the VM started with "
                "-device virtserialport,name=org.daas.printer.0 ?\n");
        if (filename) close(in_fd);
        /* STOP → CUPS holds the queue until the device comes back */
        return CUPS_BACKEND_STOP;
    }

    fprintf(stderr, "INFO: Sending job %u \"%s\" (user: %s) to SPICE client\n",
            current_job_id, title, user);

    int ret = CUPS_BACKEND_OK;

    if (send_job_start(title, user, options) < 0) {
        fprintf(stderr, "ERROR: send JOB_START failed: %s\n", strerror(errno));
        ret = CUPS_BACKEND_FAILED;
        goto out;
    }

    if (forward_job_data(in_fd) < 0) {
        abort_job();
        ret = CUPS_BACKEND_FAILED;
        goto out;
    }

    if (send_frame(DAAS_PRINT_JOB_END, NULL, 0) < 0) {
        fprintf(stderr, "ERROR: send JOB_END failed: %s\n", strerror(errno));
        ret = CUPS_BACKEND_FAILED;
        goto out;
    }

    fprintf(stderr, "INFO: Job %u sent successfully\n", current_job_id);

out:
    close(port_fd);
    port_fd = -1;
    if (filename) close(in_fd);
    return ret;
}
