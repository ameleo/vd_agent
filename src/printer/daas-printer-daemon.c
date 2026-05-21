/*
 * daas-printer-daemon — IPP tunnel daemon
 *
 * Rôle :
 *   1. Reçoit la frame PRINTER_LIST du client Windows (via virtio).
 *      Chaque entrée contient le nom d'affichage et l'URI IPP complète
 *      (ex: ipp://192.168.1.78:631/ipp/print).
 *   2. Pour chaque imprimante, ouvre un port TCP local (IPP_PORT_BASE + i)
 *      et crée / met à jour la file CUPS correspondante avec cet URI local
 *      (ex: ipp://127.0.0.1:8631/ipp/print).
 *   3. Quand CUPS se connecte sur un port local, envoie une frame
 *      DAAS_TCP_CONNECT au client avec l'URI IPP cible, puis relaie
 *      le flux TCP brut via des frames DAAS_TCP_DATA.
 *   4. Le client Windows ouvre une connexion TCP directe vers l'imprimante
 *      physique et fait un tunnel transparent (uniquement le header Host
 *      est réécrit).
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra -o daas-printer-daemon daas-printer-daemon.c
 * Install:
 *   sudo install -m 0755 daas-printer-daemon /usr/local/sbin/
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <endian.h>

#include "print-protocol.h"

/* ── Constantes ──────────────────────────────────────────────────────────── */

#define VIRTIO_PORT     "/dev/virtio-ports/org.daas.printer.0"
#define CACHE_FILE      "/run/daas-printers.cache"
#define IPP_PORT_BASE   8631          /* premier port TCP local (+ i par imprimante) */
#define MAX_PRINTERS    8             /* imprimantes simultanées max                 */
#define MAX_CUPS_CONNS  32            /* connexions CUPS simultanées max             */
#define FRAME_HDR       13            /* sizeof(daas_print_frame) = 4+4+1+4         */
#define BUF_MAX         (4 * 1024 * 1024)
#define TCP_BUF         (64 * 1024)

typedef struct {
    int      listen_fd;
    uint16_t port;
    char     cups_name[256];   /* DaaS_<safe> */
    char     uri[512];         /* ipp://192.168.1.78:631/ipp/print */
    int      active;
} printer_t;

typedef struct {
    int      fd;
    uint32_t conn_id;
    int      printer_idx;
} cups_conn_t;

static printer_t  printers[MAX_PRINTERS];
static int        n_printers = 0;

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

static void sanitize(const char *src, char *dst, size_t maxlen)
{
    size_t i = 0;
    while (*src && i < maxlen - 1) {
        char c = *src++;
        dst[i++] = (isalnum((unsigned char)c) || c == '-') ? c : '_';
    }
    dst[i] = '\0';
}

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

/* ── Envoi d'une frame sur le port virtio ────────────────────────────────── */

static int send_frame(int virtio_fd, uint8_t type, uint32_t conn_id,
                      const void *payload, uint32_t size)
{
    struct daas_print_frame hdr;
    hdr.magic   = htole32(DAAS_PRINT_MAGIC);
    hdr.conn_id = htole32(conn_id);
    hdr.type    = type;
    hdr.size    = htole32(size);

    if (write_all(virtio_fd, &hdr, FRAME_HDR) < 0) return -1;
    if (size && write_all(virtio_fd, payload, size) < 0) return -1;
    return 0;
}

/* ── Listener TCP sur un port local ─────────────────────────────────────── */

static int create_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Enregistrement CUPS ─────────────────────────────────────────────────── */

static void cups_register(const char *display_name, const char *cups_name,
                           const char *ipp_uri, int is_default)
{
    char *add_args[] = {
        "lpadmin", "-p", (char *)cups_name, "-v", (char *)ipp_uri, "-E",
        "-m", "everywhere",
        "-D", (char *)display_name, NULL
    };
    run(add_args);

    if (is_default) {
        char *def_args[] = { "lpadmin", "-d", (char *)cups_name, NULL };
        run(def_args);
    }

    fprintf(stderr, "daas-printer: %s '%s' → %s → %s\n",
            is_default ? "★" : " ", display_name, cups_name, ipp_uri);
}

/* ── Traitement frame PRINTER_LIST ───────────────────────────────────────── */

static void handle_printer_list(int virtio_fd, const uint8_t *payload,
                                 uint32_t size)
{
    (void)virtio_fd;

    if (size < 2) return;

    uint16_t count;
    memcpy(&count, payload, 2);
    count = le16toh(count);

    /* Fermer les anciens listeners */
    for (int i = 0; i < n_printers; i++) {
        if (printers[i].listen_fd >= 0) {
            close(printers[i].listen_fd);
            printers[i].listen_fd = -1;
        }
    }
    n_printers = 0;

    FILE *cache = fopen(CACHE_FILE ".tmp", "w");

    const uint8_t *p   = payload + 2;
    const uint8_t *end = payload + size;

    fprintf(stderr, "daas-printer: PRINTER_LIST — %u imprimante(s)\n", count);

    for (uint16_t i = 0; i < count && n_printers < MAX_PRINTERS; i++) {
        if (p + 3 > end) break;

        uint8_t  flags = *p++;

        /* name */
        uint16_t name_len;
        memcpy(&name_len, p, 2); p += 2;
        name_len = le16toh(name_len);
        if (p + name_len > end) break;

        char display[506];
        size_t nlen = name_len < sizeof(display) - 1 ? name_len : sizeof(display) - 1;
        memcpy(display, p, nlen); display[nlen] = '\0';
        p += name_len;

        /* uri */
        if (p + 2 > end) break;
        uint16_t uri_len;
        memcpy(&uri_len, p, 2); p += 2;
        uri_len = le16toh(uri_len);
        if (p + uri_len > end) break;

        char remote_uri[506];
        size_t ulen = uri_len < sizeof(remote_uri) - 1 ? uri_len : sizeof(remote_uri) - 1;
        memcpy(remote_uri, p, ulen); remote_uri[ulen] = '\0';
        p += uri_len;

        /* Extraire le chemin de l'URI pour construire l'URI locale */
        const char *ipp_path = "/ipp/print";   /* défaut IPP Everywhere */
        const char *slash = strstr(remote_uri, "://");
        if (slash) {
            const char *path_start = strchr(slash + 3, '/');
            if (path_start) ipp_path = path_start;
        }

        char safe[251];
        sanitize(display, safe, sizeof(safe));

        int     idx  = n_printers++;
        uint16_t port = (uint16_t)(IPP_PORT_BASE + idx);

        /* URI locale : ipp://127.0.0.1:<port><path> */
        char local_uri[512];
        snprintf(local_uri, sizeof(local_uri),
                 "ipp://127.0.0.1:%u%s", port, ipp_path);

        snprintf(printers[idx].cups_name, sizeof(printers[idx].cups_name),
                 "DaaS_%s", safe);
        snprintf(printers[idx].uri, sizeof(printers[idx].uri),
                 "%s", remote_uri);
        printers[idx].port   = port;
        printers[idx].active = 1;

        printers[idx].listen_fd = create_listener(port);
        if (printers[idx].listen_fd < 0) {
            fprintf(stderr, "daas-printer: impossible d'écouter sur :%u — %s\n",
                    port, strerror(errno));
            n_printers--;
            continue;
        }

        int is_default = (flags & DAAS_PRINTER_FLAG_DEFAULT) != 0;
        cups_register(display, printers[idx].cups_name, local_uri, is_default);

        if (cache)
            fprintf(cache, "%s\t%s\t%s\t%d\n",
                    printers[idx].cups_name, display, remote_uri, is_default);
    }

    if (cache) { fclose(cache); rename(CACHE_FILE ".tmp", CACHE_FILE); }
}

/* ── Boucle principale ───────────────────────────────────────────────────── */

static void read_loop(int virtio_fd)
{
    uint8_t *vbuf = malloc(BUF_MAX);
    uint8_t *tbuf = malloc(TCP_BUF);
    if (!vbuf || !tbuf) { perror("malloc"); free(vbuf); free(tbuf); return; }

    size_t vlen = 0;

    cups_conn_t conns[MAX_CUPS_CONNS];
    for (int i = 0; i < MAX_CUPS_CONNS; i++) conns[i].fd = -1;
    uint32_t next_id = 1;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(virtio_fd, &rfds);
        int maxfd = virtio_fd;

        /* Listeners par imprimante */
        for (int i = 0; i < n_printers; i++) {
            if (printers[i].listen_fd >= 0) {
                FD_SET(printers[i].listen_fd, &rfds);
                if (printers[i].listen_fd > maxfd) maxfd = printers[i].listen_fd;
            }
        }

        /* Connexions CUPS actives */
        for (int i = 0; i < MAX_CUPS_CONNS; i++) {
            if (conns[i].fd >= 0) {
                FD_SET(conns[i].fd, &rfds);
                if (conns[i].fd > maxfd) maxfd = conns[i].fd;
            }
        }

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("daas-printer: select");
            break;
        }

        /* Nouvelle connexion CUPS sur un des listeners */
        for (int pi = 0; pi < n_printers; pi++) {
            if (printers[pi].listen_fd < 0 ||
                !FD_ISSET(printers[pi].listen_fd, &rfds)) continue;

            int new_fd = accept(printers[pi].listen_fd, NULL, NULL);
            if (new_fd < 0) continue;

            int slot = -1;
            for (int i = 0; i < MAX_CUPS_CONNS; i++) {
                if (conns[i].fd < 0) { slot = i; break; }
            }
            if (slot < 0) {
                fprintf(stderr, "daas-printer: trop de connexions, refus\n");
                close(new_fd);
                continue;
            }

            uint32_t cid = next_id++;
            conns[slot].fd          = new_fd;
            conns[slot].conn_id     = cid;
            conns[slot].printer_idx = pi;

            /* Informer le client de l'URI cible avant tout TCP_DATA */
            const char *uri = printers[pi].uri;
            send_frame(virtio_fd, DAAS_TCP_CONNECT, cid,
                       uri, (uint32_t)strlen(uri));

            fprintf(stderr, "daas-printer: connexion CUPS conn_id=%u → %s\n",
                    cid, uri);
        }

        /* Données CUPS → frames DAAS_TCP_DATA → virtio */
        for (int i = 0; i < MAX_CUPS_CONNS; i++) {
            if (conns[i].fd < 0 || !FD_ISSET(conns[i].fd, &rfds)) continue;
            ssize_t n = read(conns[i].fd, tbuf, TCP_BUF);
            if (n <= 0) {
                send_frame(virtio_fd, DAAS_TCP_CLOSE, conns[i].conn_id, NULL, 0);
                close(conns[i].fd);
                conns[i].fd = -1;
                fprintf(stderr, "daas-printer: CUPS conn_id=%u fermée\n",
                        conns[i].conn_id);
            } else {
                send_frame(virtio_fd, DAAS_TCP_DATA, conns[i].conn_id,
                           tbuf, (uint32_t)n);
            }
        }

        /* Données virtio → parser frames → router */
        if (!FD_ISSET(virtio_fd, &rfds)) continue;

        ssize_t n = read(virtio_fd, vbuf + vlen, BUF_MAX - vlen);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            fprintf(stderr, "daas-printer: port virtio fermé\n");
            break;
        }
        vlen += (size_t)n;

        size_t off = 0;
        while (off + FRAME_HDR <= vlen) {
            uint32_t magic;
            memcpy(&magic, vbuf + off, 4);
            if (le32toh(magic) != DAAS_PRINT_MAGIC) { off++; continue; }

            uint32_t conn_id;
            uint8_t  type;
            uint32_t psize;
            memcpy(&conn_id, vbuf + off + 4, 4); conn_id = le32toh(conn_id);
            memcpy(&type,    vbuf + off + 8, 1);
            memcpy(&psize,   vbuf + off + 9, 4); psize   = le32toh(psize);

            if (off + FRAME_HDR + psize > vlen) break;

            const uint8_t *payload = vbuf + off + FRAME_HDR;

            if (type == DAAS_PRINT_PRINTER_LIST) {
                handle_printer_list(virtio_fd, payload, psize);
            } else if (type == DAAS_TCP_DATA) {
                for (int i = 0; i < MAX_CUPS_CONNS; i++) {
                    if (conns[i].fd >= 0 && conns[i].conn_id == conn_id) {
                        write_all(conns[i].fd, payload, psize);
                        break;
                    }
                }
            } else if (type == DAAS_TCP_CLOSE) {
                for (int i = 0; i < MAX_CUPS_CONNS; i++) {
                    if (conns[i].fd >= 0 && conns[i].conn_id == conn_id) {
                        close(conns[i].fd);
                        conns[i].fd = -1;
                        fprintf(stderr, "daas-printer: TCP_CLOSE conn_id=%u\n",
                                conn_id);
                        break;
                    }
                }
            }

            off += FRAME_HDR + psize;
        }

        if (off > 0 && off < vlen)
            memmove(vbuf, vbuf + off, vlen - off);
        vlen -= off;
    }

    for (int i = 0; i < MAX_CUPS_CONNS; i++)
        if (conns[i].fd >= 0) close(conns[i].fd);
    free(vbuf);
    free(tbuf);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    fprintf(stderr, "daas-printer-daemon: démarré (ports %d–%d)\n",
            IPP_PORT_BASE, IPP_PORT_BASE + MAX_PRINTERS - 1);

    for (int i = 0; i < MAX_PRINTERS; i++) printers[i].listen_fd = -1;

    while (1) {
        int fd = open(VIRTIO_PORT, O_RDWR);
        if (fd < 0) {
            if (errno != ENOENT && errno != ENXIO)
                perror("daas-printer: open");
            sleep(5);
            continue;
        }
        fprintf(stderr, "daas-printer: port virtio ouvert\n");
        read_loop(fd);
        close(fd);

        /* Fermer les listeners à la reconnexion (le client renverra PRINTER_LIST) */
        for (int i = 0; i < n_printers; i++) {
            if (printers[i].listen_fd >= 0) {
                close(printers[i].listen_fd);
                printers[i].listen_fd = -1;
            }
        }
        n_printers = 0;

        sleep(2);
    }

    return 0;
}
