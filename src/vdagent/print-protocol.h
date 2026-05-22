/* print-protocol.h — Wire protocol for DaaS SPICE printer channel
 *
 * Transport : virtio-serial port "org.daas.printer.0"
 *             Linux  : /dev/virtio-ports/org.daas.printer.0
 *             Windows: \\.\Global\org.daas.printer.0
 *
 * Byte order : little-endian throughout
 *
 * Frame layout (all fields LE):
 *   [magic:4][conn_id:4][type:1][size:4]  followed by <size> bytes of payload
 *
 * ── TCP_DATA payload (bidirectional) ─────────────────────────────────────────
 *   Raw TCP bytes belonging to the IPP connection identified by conn_id.
 *   conn_id = 0 is reserved; valid IDs start at 1.
 *
 * ── TCP_CLOSE payload ────────────────────────────────────────────────────────
 *   empty (size = 0) — peer closed the TCP connection
 *
 * ── PRINTER_LIST payload (client → VM) ───────────────────────────────────────
 *   Sent once at channel open, and again whenever the local printer list
 *   changes.  The VM side caches the list so CUPS queues can be (re)created.
 *
 *   [count : 2 LE]
 *   for each printer:
 *     [flags    : 1]       bit 0 = is_default
 *     [name_len : 2 LE]
 *     [name     : name_len bytes, UTF-8, no NUL terminator]
 *     [uri_len  : 2 LE]
 *     [uri      : uri_len bytes, UTF-8, no NUL terminator]
 *                          e.g. "ipp://192.168.1.78:631/ipp/print"
 *
 * ── TCP_CONNECT payload (VM → client) ────────────────────────────────────────
 *   Sent by the VM before the first TCP_DATA of a new CUPS connection.
 *   Payload : the IPP URI of the target printer (UTF-8, no NUL).
 *   The client uses this URI to open a direct TCP connection to the printer.
 */

#ifndef DAAS_PRINT_PROTOCOL_H
#define DAAS_PRINT_PROTOCOL_H

#include <stdint.h>

#define DAAS_PRINT_MAGIC    0x44414153U   /* "DAAS" stored LE */
#define DAAS_PRINT_VERSION  2

/* ── Frame types ─────────────────────────────────────────────────────────── */
#define DAAS_TCP_DATA            0x10  /* bidirectional : raw IPP/TCP bytes      */
#define DAAS_TCP_CLOSE           0x11  /* bidirectional : TCP connection closed  */
#define DAAS_TCP_CONNECT         0x12  /* VM → client   : new CUPS connection    */
#define DAAS_PRINT_PRINTER_LIST     6  /* client → VM   : available printers     */

/* ── Printer flags (flags byte in PRINTER_LIST entries) ──────────────────── */
#define DAAS_PRINTER_FLAG_DEFAULT  0x01

/* ── On-wire frame header ─────────────────────────────────────────────────── */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct daas_print_frame {
    uint32_t magic;    /* DAAS_PRINT_MAGIC, LE                            */
    uint32_t conn_id;  /* TCP connection ID (0 = broadcast/control), LE   */
    uint8_t  type;     /* DAAS_TCP_DATA / DAAS_TCP_CLOSE / PRINTER_LIST   */
    uint32_t size;     /* payload size, LE                                */
}
#ifdef _MSC_VER
;
#pragma pack(pop)
#else
__attribute__((packed));
#endif

#endif /* DAAS_PRINT_PROTOCOL_H */
