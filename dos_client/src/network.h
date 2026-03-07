/*
 * RetroSurf Network Layer - Watt-32 TCP/IP
 *
 * Handles TCP connection to the rendering server,
 * message framing, send/receive with the binary protocol.
 */

#ifndef RETROSURF_NETWORK_H
#define RETROSURF_NETWORK_H

#include <stdint.h>
#include "protocol.h"

/* Maximum payload size we can receive */
#define MAX_PAYLOAD_SIZE  65000

/* Receive buffer size (header + max payload) */
#define RECV_BUF_SIZE     (HEADER_SIZE + MAX_PAYLOAD_SIZE)

/* Connection state */
#define NET_DISCONNECTED  0
#define NET_CONNECTING    1
#define NET_CONNECTED     2
#define NET_ERROR         3

/* Network context */
typedef struct {
    int             state;
    int             socket_fd;
    uint32_t        server_ip;
    uint16_t        server_port;

    /* Receive buffer for assembling messages */
    uint8_t         recv_buf[RECV_BUF_SIZE];
    int             recv_pos;       /* bytes currently in recv_buf */
    int             recv_need;      /* bytes needed to complete current read */

    /* Last error message */
    char            error_msg[128];
} net_context_t;

/* Initialize the Watt-32 TCP/IP stack.
 * Returns 0 on success, -1 on failure. */
int net_init(void);

/* Shut down the TCP/IP stack. */
void net_shutdown(void);

/* Connect to the server.
 * server_ip: dotted-quad string like "10.0.2.2"
 * port: TCP port (8086)
 * Returns 0 on success, -1 on failure. */
int net_connect(net_context_t *ctx, const char *server_ip, uint16_t port);

/* Close the connection. */
void net_close(net_context_t *ctx);

/* Send a complete protocol message (header + payload).
 * Returns 0 on success, -1 on failure. */
int net_send_message(net_context_t *ctx, uint8_t msg_type,
                     const uint8_t *payload, uint16_t payload_len);

/* Try to receive a complete message (non-blocking).
 * Reads available data and assembles messages.
 * Returns:
 *   1  = complete message available in *header and *payload, *payload_len
 *   0  = no complete message yet (call again later)
 *  -1  = error or disconnection
 *
 * payload must point to a buffer of at least MAX_PAYLOAD_SIZE bytes.
 */
int net_recv_message(net_context_t *ctx, msg_header_t *header,
                     uint8_t *payload, uint16_t *payload_len);

/* Poll for network events. Call this periodically from the main loop.
 * Drives the Watt-32 TCP/IP stack. */
void net_poll(void);

/* Check if TCP data is available for reading.
 * Returns 1 if data ready, 0 if not. */
int net_data_ready(void);

#endif /* RETROSURF_NETWORK_H */
