/*
 * daas-printer-daemon — lit le port virtio-serial org.daas.printer.0,
 * parse les frames PRINTER_LIST et maintient les files CUPS correspondantes.
 *
 * Le daemon agit aussi comme proxy pour le backend CUPS spice : il écoute
 * sur DAAS_PRINTER_PROXY_SOCK et relaie les frames JOB_* vers le port
 * virtio, évitant ainsi le conflit d'ouverture exclusive du char device.
 *
 * Compile: gcc -O2 -o daas-printer-daemon daas-printer-daemon.c
 * Install: sudo install -m 0755 daas-printer-daemon /usr/local/sbin/
 * Run:     sudo daas-printer-daemon   (ou via systemd)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <endian.h>

#include "print-protocol.h"

/* ── Constantes ──────────────────────────────────────────────────────────── */

#define VIRTIO_PORT  "/dev/virtio-ports/org.daas.printer.0"
#define CACHE_FILE   "/run/daas-printers.cache"
#define FRAME_HDR    13   /* 4+4+1+4 */
#define BUF_MAX      (4 * 1024 * 1024)   /* 4 Mo — limite anti-burst */

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
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* Convertit un nom d'imprimante en nom CUPS valide (alphanum + tirets). */
static void sanitize(const char *src, char *dst, size_t maxlen)
{
    size_t i = 0;
    while (*src && i < maxlen - 1) {
        char c = *src++;
        dst[i++] = (isalnum((unsigned char)c) || c == '-') ? c : '_';
    }
    dst[i] = '\0';
}

/* Exécute une commande via fork/execvp (évite l'injection shell). */
static int run(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── Enregistrement CUPS ─────────────────────────────────────────────────── */

static void cups_register(const char *display_name, const char *safe_name,
                           int is_default)
{
    /* "DaaS_" (5) + safe_name (≤250) + '\0' = 256 */
    char cups_name[256];
    snprintf(cups_name, sizeof(cups_name), "DaaS_%s", safe_name);

    char uri[512];
    snprintf(uri, sizeof(uri), "spice:///%s", safe_name);

    /* "DaaS: " (6) + display_name (≤505) + '\0' = 512 */
    char desc[512];
    snprintf(desc, sizeof(desc), "DaaS: %s", display_name);

    char *add_args[] = {
        "lpadmin", "-p", cups_name, "-v", uri, "-E",
        "-D", desc, NULL
    };
    run(add_args);

    if (is_default) {
        char *def_args[] = { "lpadmin", "-d", cups_name, NULL };
        run(def_args);
    }

    fprintf(stderr, "daas-printer: %s '%s' → %s\n",
            is_default ? "★" : " ", display_name, cups_name);
}

/* ── Traitement frame PRINTER_LIST ───────────────────────────────────────── */

static void handle_printer_list(const uint8_t *payload, uint32_t size)
{
    if (size < 2) return;

    uint16_t count;
    memcpy(&count, payload, 2);
    count = le16toh(count);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", CACHE_FILE);
    FILE *cache = fopen(tmp_path, "w");
    if (!cache) {
        perror("daas-printer: fopen cache");
        return;
    }

    const uint8_t *p   = payload + 2;
    const uint8_t *end = payload + size;

    fprintf(stderr, "daas-printer: PRINTER_LIST — %u imprimante(s)\n", count);

    for (uint16_t i = 0; i < count; i++) {
        if (p + 3 > end) break;

        uint8_t  flags    = *p++;
        uint16_t name_len;
        memcpy(&name_len, p, 2); p += 2;
        name_len = le16toh(name_len);

        if (p + name_len > end) break;

        /* 505 chars max so "DaaS: " + display fits in the 512-byte desc buffer */
        char display[506];
        size_t nlen = name_len < sizeof(display) - 1 ? name_len : sizeof(display) - 1;
        memcpy(display, p, nlen);
        display[nlen] = '\0';
        p += name_len;

        /* 250 chars max so "DaaS_" + safe fits in the 256-byte cups_name buffer */
        char safe[251];
        sanitize(display, safe, sizeof(safe));

        int is_default = (flags & DAAS_PRINTER_FLAG_DEFAULT) != 0;

        fprintf(cache, "DaaS_%s\t%s\t%d\n", safe, display, is_default);

        cups_register(display, safe, is_default);
    }

    fclose(cache);
    rename(tmp_path, CACHE_FILE);
}

/* ── Proxy socket ────────────────────────────────────────────────────────── */

static int create_proxy_sock(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("daas-printer: socket"); return -1; }

    unlink(DAAS_PRINTER_PROXY_SOCK);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, DAAS_PRINTER_PROXY_SOCK, sizeof(sa.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 4) < 0) {
        perror("daas-printer: bind/listen proxy");
        close(fd);
        return -1;
    }
    /* Writable par root (le backend CUPS s'exécute en root). */
    chmod(DAAS_PRINTER_PROXY_SOCK, 0600);
    return fd;
}

/* ── Boucle principale : virtio + proxy ─────────────────────────────────── */

static void read_loop(int virtio_fd, int proxy_fd)
{
    uint8_t *buf  = malloc(BUF_MAX);
    size_t   blen = 0;
    int      client_fd = -1;

    if (!buf) { perror("malloc"); return; }

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(virtio_fd, &rfds);
        FD_SET(proxy_fd,  &rfds);
        if (client_fd >= 0) FD_SET(client_fd, &rfds);

        int maxfd = virtio_fd > proxy_fd ? virtio_fd : proxy_fd;
        if (client_fd > maxfd) maxfd = client_fd;

        int r = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("daas-printer: select");
            break;
        }

        /* Nouvelle connexion d'un backend CUPS */
        if (FD_ISSET(proxy_fd, &rfds)) {
            int new_fd = accept(proxy_fd, NULL, NULL);
            if (new_fd >= 0) {
                if (client_fd >= 0) close(client_fd);
                client_fd = new_fd;
                fprintf(stderr, "daas-printer: backend CUPS connecté\n");
            }
        }

        /* Données du backend → relayer vers le port virtio */
        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            uint8_t fwd[65536];
            ssize_t n = read(client_fd, fwd, sizeof(fwd));
            if (n <= 0) {
                fprintf(stderr, "daas-printer: backend déconnecté\n");
                close(client_fd);
                client_fd = -1;
            } else {
                if (write_all(virtio_fd, fwd, (size_t)n) < 0)
                    fprintf(stderr, "daas-printer: write virtio: %s\n",
                            strerror(errno));
            }
        }

        /* Données du port virtio → parse les frames */
        if (FD_ISSET(virtio_fd, &rfds)) {
            ssize_t n = read(virtio_fd, buf + blen, BUF_MAX - blen);
            if (n <= 0) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                fprintf(stderr, "daas-printer: port fermé (read=%zd)\n", n);
                break;
            }
            blen += (size_t)n;

            size_t off = 0;
            while (off + FRAME_HDR <= blen) {
                uint32_t magic;
                memcpy(&magic, buf + off, 4);
                if (le32toh(magic) != DAAS_PRINT_MAGIC) {
                    off++;
                    continue;
                }

                uint8_t  type;
                uint32_t payload_size;
                memcpy(&type,         buf + off + 8, 1);
                memcpy(&payload_size, buf + off + 9, 4);
                payload_size = le32toh(payload_size);

                if (off + FRAME_HDR + payload_size > blen) break;

                if (type == DAAS_PRINT_PRINTER_LIST)
                    handle_printer_list(buf + off + FRAME_HDR, payload_size);

                off += FRAME_HDR + payload_size;
            }

            if (off > 0 && off < blen)
                memmove(buf, buf + off, blen - off);
            blen -= off;
        }
    }

    if (client_fd >= 0) close(client_fd);
    free(buf);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    fprintf(stderr, "daas-printer-daemon: démarré, attente de %s\n", VIRTIO_PORT);

    int proxy_fd = create_proxy_sock();
    if (proxy_fd < 0) {
        fprintf(stderr, "daas-printer: impossible de créer le proxy socket\n");
        return 1;
    }
    fprintf(stderr, "daas-printer: proxy socket prêt sur %s\n",
            DAAS_PRINTER_PROXY_SOCK);

    while (1) {
        int fd = open(VIRTIO_PORT, O_RDWR);
        if (fd < 0) {
            if (errno != ENOENT && errno != ENXIO)
                perror("daas-printer: open");
            sleep(5);
            continue;
        }

        fprintf(stderr, "daas-printer: port ouvert\n");
        read_loop(fd, proxy_fd);
        close(fd);

        sleep(2);
    }

    close(proxy_fd);
    unlink(DAAS_PRINTER_PROXY_SOCK);
    return 0;
}
