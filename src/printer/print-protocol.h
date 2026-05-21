/* print-protocol.h — Wire protocol for DaaS SPICE printer channel
 *
 * Transport : virtio-serial port "org.daas.printer.0"
 *             Linux  : /dev/virtio-ports/org.daas.printer.0
 *             Windows: \\.\Global\org.daas.printer.0
 *
 * Byte order : little-endian throughout
 *
 * Frame layout (all fields LE):
 *   [magic:4][job_id:4][type:1][size:4]  followed by <size> bytes of payload
 *
 * ── JOB_START payload (VM → client) ────────────────────────────────────────
 *   [format:1][title_len:2][user_len:2][options_len:2]
 *   [title : title_len bytes]
 *   [user  : user_len  bytes]
 *   [options : options_len bytes]   (CUPS key=value space-separated string)
 *
 * ── JOB_DATA payload (VM → client) ──────────────────────────────────────────
 *   raw document bytes (PDF / PS / PWG-Raster / raw)
 *
 * ── JOB_END / JOB_ABORT payload ─────────────────────────────────────────────
 *   empty (size = 0)
 *
 * ── JOB_STATUS payload (client → VM) ────────────────────────────────────────
 *   [code:1][msg_len:1][msg : msg_len bytes]
 *
 * ── PRINTER_LIST payload (client → VM) ───────────────────────────────────────
 *   Sent once at channel open, and again whenever the local printer list
 *   changes.  The VM side caches the list so the CUPS backend can present
 *   a printer picker to the user.
 *
 *   [count : 2 LE]
 *   for each printer:
 *     [flags    : 1]       bit 0 = is_default
 *     [name_len : 2 LE]
 *     [name     : name_len bytes, UTF-8, no NUL terminator]
 */

#ifndef DAAS_PRINT_PROTOCOL_H
#define DAAS_PRINT_PROTOCOL_H

#include <stdint.h>

/* Unix socket used by daas-printer-daemon to proxy jobs from the CUPS backend */
#define DAAS_PRINTER_PROXY_SOCK  "/run/daas-printer-proxy.sock"

#define DAAS_PRINT_MAGIC    0x44414153U   /* "DAAS" stored LE */
#define DAAS_PRINT_VERSION  1

/* ── Frame types ─────────────────────────────────────────────────────────── */
#define DAAS_PRINT_JOB_START      1   /* VM → client : begin job + metadata  */
#define DAAS_PRINT_JOB_DATA       2   /* VM → client : document data chunk   */
#define DAAS_PRINT_JOB_END        3   /* VM → client : job finished          */
#define DAAS_PRINT_JOB_ABORT      4   /* VM → client : cancel in-flight job  */
#define DAAS_PRINT_JOB_STATUS     5   /* client → VM : result / ACK          */
#define DAAS_PRINT_PRINTER_LIST   6   /* client → VM : available printers    */

/* ── Document formats (format byte in JOB_START) ─────────────────────────── */
#define DAAS_PRINT_FMT_PDF         0
#define DAAS_PRINT_FMT_PS          1
#define DAAS_PRINT_FMT_PWG_RASTER  2
#define DAAS_PRINT_FMT_RAW         255

/* ── Status codes (code byte in JOB_STATUS) ──────────────────────────────── */
#define DAAS_PRINT_STATUS_OK          0   /* job accepted / printed */
#define DAAS_PRINT_STATUS_ERROR       1   /* generic error          */
#define DAAS_PRINT_STATUS_NO_PRINTER  2   /* no local printer found */

/* ── Printer flags (flags byte in PRINTER_LIST entries) ──────────────────── */
#define DAAS_PRINTER_FLAG_DEFAULT     0x01

/* ── On-wire frame header ─────────────────────────────────────────────────── */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct daas_print_frame {
    uint32_t magic;    /* DAAS_PRINT_MAGIC, LE */
    uint32_t job_id;   /* LE                   */
    uint8_t  type;     /* DAAS_PRINT_JOB_*     */
    uint32_t size;     /* payload size, LE     */
}
#ifdef _MSC_VER
;
#pragma pack(pop)
#else
__attribute__((packed));
#endif

#endif /* DAAS_PRINT_PROTOCOL_H */
