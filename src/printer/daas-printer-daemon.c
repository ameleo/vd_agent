/*
 * daas-printer-daemon — IPP tunnel daemon
 *
 * Rôle :
 *   1. Lit le port virtio org.daas.printer.0.
 *   2. Traite les frames PRINTER_LIST venant du client Windows et crée /
 *      met à jour les files CUPS correspondantes avec l'URI
 *      ipp://127.0.0.1:IPP_PORT/printers/<safe_name>.
 *   3. Écoute sur IPP_PORT (TCP, localhost) ; quand CUPS s'y connecte pour
 *      envoyer un job IPP, relaie le flux TCP brut vers le client via des
 *      frames DAAS_TCP_DATA sur le port virtio, et retourne les réponses.
 *
 * Le client Windows reçoit les frames TCP_DATA et les relaie directement
 * vers l'endpoint IPP natif de l'imprimante physique (HP OfficeJet Pro 9010
 * supporte IPP Everywhere sur ipp://<ip>:631/ipp/print).
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
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <endian.h>

#include "print-protocol.h"

/* ── Constantes ──────────────────────────────────────────────────────────── */

#define VIRTIO_PORT   "/dev/virtio-ports/org.daas.printer.0"
#define CACHE_FILE    "/run/daas-printers.cache"
#define IPP_PORT      8631          /* TCP localhost, écouté par le daemon  */
#define FRAME_HDR     13            /* sizeof(daas_print_frame) = 4+4+1+4   */
#define BUF_MAX       (4 * 1024 * 1024)
#define TCP_BUF       (64 * 1024)

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

/* ── Enregistrement CUPS (URI ipp://127.0.0.1:IPP_PORT/printers/<name>) ─── */

static void cups_register(const char *display_name, const char *safe_name,
                           int is_default)
{
    char cups_name[256];
    snprintf(cups_name, sizeof(cups_name), "DaaS_%s", safe_name);

    char uri[512];
    snprintf(uri, sizeof(uri), "ipp://127.0.0.1:%d/printers/%s",
             IPP_PORT, safe_name);

    char desc[512];
    snprintf(desc, sizeof(desc), "DaaS: %s", display_name);

    char *add_args[] = {
        "lpadmin", "-p", cups_name, "-v", uri, "-E",
        "-m", "everywhere",          /* IPP Everywhere — pas de PPD requis */
        "-D", desc, NULL
    };
    run(add_args);

    if (is_default) {
        char *def_args[] = { "lpadmin", "-d", cups_name, NULL };
        run(def_args);
    }

    fprintf(stderr, "daas-printer: %s '%s' → %s → %s\n",
            is_default ? "★" : " ", display_name, cups_name, uri);
}

/* ── Traitement frame PRINTER_LIST ───────────────────────────────────────── */

static void handle_printer_list(int virtio_fd, const uint8_t *payload,
                                 uint32_t size)
{
    if (size < 2) return;

    uint16_t count;
    memcpy(&count, payload, 2);
    count = le16toh(count);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", CACHE_FILE);
    FILE *cache = fopen(tmp_path, "w");
    if (!cache) { perror("daas-printer: fopen cache"); return; }

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

        char display[506];
        size_t nlen = name_len < sizeof(display) - 1 ? name_len : sizeof(display) - 1;
        memcpy(display, p, nlen);
        display[nlen] = '\0';
        p += name_len;

        char safe[251];
        sanitize(display, safe, sizeof(safe));

        int is_default = (flags & DAAS_PRINTER_FLAG_DEFAULT) != 0;
        fprintf(cache, "DaaS_%s\t%s\t%d\n", safe, display, is_default);
        cups_register(display, safe, is_default);
    }

    fclose(cache);
    rename(tmp_path, CACHE_FILE);

    (void)virtio_fd; /* pas de réponse nécessaire */
}

/* ── Listener TCP localhost:IPP_PORT ─────────────────────────────────────── */

static int create_ipp_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("daas-printer: socket IPP"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(IPP_PORT);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 8) < 0) {
        perror("daas-printer: bind/listen IPP");
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Boucle principale ───────────────────────────────────────────────────── */

/*
 * Modèle simplifié : une seule connexion CUPS active à la fois.
 * conn_id = 1 pour toute connexion (pas de multiplexage nécessaire :
 * CUPS sérialise les jobs sur une file).
 */
static void read_loop(int virtio_fd, int ipp_listen_fd)
{
    uint8_t *vbuf  = malloc(BUF_MAX);
    uint8_t *tbuf  = malloc(TCP_BUF);
    if (!vbuf || !tbuf) { perror("malloc"); free(vbuf); free(tbuf); return; }

    size_t vlen      = 0;     /* octets accumulés depuis virtio */
    int    cups_fd   = -1;    /* connexion CUPS active           */

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(virtio_fd,    &rfds);
        FD_SET(ipp_listen_fd, &rfds);
        if (cups_fd >= 0) FD_SET(cups_fd, &rfds);

        int maxfd = virtio_fd > ipp_listen_fd ? virtio_fd : ipp_listen_fd;
        if (cups_fd > maxfd) maxfd = cups_fd;

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("daas-printer: select");
            break;
        }

        /* Nouvelle connexion CUPS */
        if (FD_ISSET(ipp_listen_fd, &rfds)) {
            int new_fd = accept(ipp_listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                if (cups_fd >= 0) close(cups_fd);
                cups_fd = new_fd;
                fprintf(stderr, "daas-printer: connexion CUPS (IPP)\n");
            }
        }

        /* Données CUPS → encapsuler en frame TCP_DATA → virtio */
        if (cups_fd >= 0 && FD_ISSET(cups_fd, &rfds)) {
            ssize_t n = read(cups_fd, tbuf, TCP_BUF);
            if (n <= 0) {
                fprintf(stderr, "daas-printer: CUPS fermé (n=%zd)\n", n);
                send_frame(virtio_fd, DAAS_TCP_CLOSE, 1, NULL, 0);
                close(cups_fd);
                cups_fd = -1;
            } else {
                if (send_frame(virtio_fd, DAAS_TCP_DATA, 1, tbuf, (uint32_t)n) < 0)
                    fprintf(stderr, "daas-printer: write virtio: %s\n",
                            strerror(errno));
            }
        }

        /* Données virtio → parser frames */
        if (FD_ISSET(virtio_fd, &rfds)) {
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

                uint8_t  type;
                uint32_t psize;
                memcpy(&type,  vbuf + off + 8, 1);
                memcpy(&psize, vbuf + off + 9, 4);
                psize = le32toh(psize);

                if (off + FRAME_HDR + psize > vlen) break;

                const uint8_t *payload = vbuf + off + FRAME_HDR;

                if (type == DAAS_PRINT_PRINTER_LIST) {
                    handle_printer_list(virtio_fd, payload, psize);
                } else if (type == DAAS_TCP_DATA) {
                    /* Réponse IPP du client → renvoyer à CUPS */
                    if (cups_fd >= 0)
                        write_all(cups_fd, payload, psize);
                } else if (type == DAAS_TCP_CLOSE) {
                    if (cups_fd >= 0) {
                        close(cups_fd);
                        cups_fd = -1;
                        fprintf(stderr, "daas-printer: TCP_CLOSE reçu du client\n");
                    }
                }

                off += FRAME_HDR + psize;
            }

            if (off > 0 && off < vlen)
                memmove(vbuf, vbuf + off, vlen - off);
            vlen -= off;
        }
    }

    if (cups_fd >= 0) close(cups_fd);
    free(vbuf);
    free(tbuf);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    fprintf(stderr, "daas-printer-daemon: démarré (IPP tunnel sur port %d)\n",
            IPP_PORT);

    int ipp_listen_fd = create_ipp_listener();
    if (ipp_listen_fd < 0) {
        fprintf(stderr, "daas-printer: impossible d'écouter sur :%d\n", IPP_PORT);
        return 1;
    }
    fprintf(stderr, "daas-printer: listener IPP prêt sur 127.0.0.1:%d\n",
            IPP_PORT);

    while (1) {
        int fd = open(VIRTIO_PORT, O_RDWR);
        if (fd < 0) {
            if (errno != ENOENT && errno != ENXIO)
                perror("daas-printer: open");
            sleep(5);
            continue;
        }
        fprintf(stderr, "daas-printer: port virtio ouvert\n");
        read_loop(fd, ipp_listen_fd);
        close(fd);
        sleep(2);
    }

    close(ipp_listen_fd);
    return 0;
}
