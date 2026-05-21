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
#include <sys/socket.h>
#include <sys/un.h>
#include <endian.h>

#include "print-protocol.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

#define VIRTIO_PORT  "/dev/virtio-ports/org.daas.printer.0"
#define CACHE_FILE   "/run/daas-printers.cache"
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

/* ── Connexion au proxy du daemon ────────────────────────────────────────── */

static int open_proxy_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, DAAS_PRINTER_PROXY_SOCK, sizeof(sa.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Helpers pour DEVICE_URI et cache ───────────────────────────────────── */

/* Retourne la partie après "spice:///" dans DEVICE_URI, ou NULL. */
static const char *printer_name_from_env(void)
{
    const char *uri = getenv("DEVICE_URI");
    if (!uri) return NULL;
    const char *p = strstr(uri, "spice:///");
    if (!p) return NULL;
    p += 9; /* strlen("spice:///") */
    return *p ? p : NULL;
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /*
     * Device discovery — CUPS calls the backend with no arguments to
     * enumerate available devices.  One device per line:
     *   class uri "make-and-model" "info" ["device-id"]
     * We read the printer cache built by daas-printer-daemon; fall back to
     * a generic entry if the cache is absent (daemon not yet running).
     */
    if (argc == 1) {
        FILE *cache = fopen(CACHE_FILE, "r");
        int   found = 0;
        if (cache) {
            char line[1024];
            while (fgets(line, (int)sizeof(line), cache)) {
                char cups_name[256], display[512];
                int  is_default = 0;
                /* format: cups_name\tdisplay_name\tis_default\n */
                if (sscanf(line, "%255[^\t]\t%511[^\t]\t%d",
                           cups_name, display, &is_default) >= 2) {
                    printf("direct spice:///%s \"%s\" \"DaaS: %s\"\n",
                           cups_name, display, display);
                    found++;
                }
            }
            fclose(cache);
        }
        if (!found)
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

    /*
     * Se connecte au proxy Unix socket géré par daas-printer-daemon.
     * Le daemon garde le virtio port ouvert (le char device est exclusif) et
     * relaie nos frames JOB_* vers le port virtio.
     */
    port_fd = open_proxy_socket();
    if (port_fd < 0) {
        fprintf(stderr, "ERROR: cannot connect to proxy %s: %s\n",
                DAAS_PRINTER_PROXY_SOCK, strerror(errno));
        fprintf(stderr, "INFO: Is daas-printer-daemon running ?\n");
        if (filename) close(in_fd);
        /* STOP → CUPS remet la file en attente jusqu'au retour du daemon */
        return CUPS_BACKEND_STOP;
    }

    fprintf(stderr, "INFO: Sending job %u \"%s\" (user: %s) to SPICE client\n",
            current_job_id, title, user);

    /*
     * Append printer=<cups_name> to the options string so the client knows
     * which physical printer to target.  CUPS sets DEVICE_URI=spice:///cups_name.
     */
    char *ext_options = NULL;
    const char *pname = printer_name_from_env();
    if (pname) {
        size_t base_len = options ? strlen(options) : 0;
        size_t plen     = strlen(pname);
        ext_options = malloc(base_len + plen + 10);
        if (ext_options) {
            if (base_len)
                snprintf(ext_options, base_len + plen + 10,
                         "%s printer=%s", options, pname);
            else
                snprintf(ext_options, plen + 9, "printer=%s", pname);
        }
    }

    int ret = CUPS_BACKEND_OK;

    if (send_job_start(title, user, ext_options ? ext_options : options) < 0) {
        fprintf(stderr, "ERROR: send JOB_START failed: %s\n", strerror(errno));
        free(ext_options);
        ret = CUPS_BACKEND_FAILED;
        goto out;
    }
    free(ext_options);

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
