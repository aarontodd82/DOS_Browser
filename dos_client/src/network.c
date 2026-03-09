/*
 * RetroSurf Network Layer - Watt-32 TCP/IP implementation
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <tcp.h>       /* Watt-32 main header */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network.h"
#include "protocol.h"

/* Watt-32 TCP socket */
static tcp_Socket tcp_sock;

/* Large receive buffer for high-throughput frame streaming.
 * Default Watt-32 rx buffer is small (~2KB), causing TCP window
 * stalls when receiving large frames. 48KB allows the server to
 * send a full frame without waiting for ACKs mid-transfer. */
#define BIG_RX_BUF_SIZE  49152
static uint8_t big_rx_buf[BIG_RX_BUF_SIZE];

int net_init(void)
{
    int rc;

    printf("Initializing TCP/IP stack...\n");
    rc = sock_init();
    if (rc != 0) {
        printf("ERROR: sock_init() failed (rc=%d)\n", rc);
        printf("Check WATTCP.CFG and packet driver.\n");
        return -1;
    }
    printf("TCP/IP stack initialized.\n");
    return 0;
}

void net_shutdown(void)
{
    sock_exit();
}

int net_connect(net_context_t *ctx, const char *server_ip, uint16_t port)
{
    DWORD ip;
    clock_t start, deadline;
    int wait_secs = 15;

    memset(ctx, 0, sizeof(net_context_t));
    ctx->state = NET_CONNECTING;
    ctx->server_port = port;

    /* Resolve IP address using Watt-32's resolve() */
    ip = resolve(server_ip);
    if (ip == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Cannot resolve: %s", server_ip);
        ctx->state = NET_ERROR;
        return -1;
    }
    ctx->server_ip = ip;

    printf("Connecting to %s:%u...\n", server_ip, port);

    /* Open TCP connection using Watt-32 API */
    if (!tcp_open(&tcp_sock, 0, ip, port, NULL)) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "tcp_open() failed");
        ctx->state = NET_ERROR;
        return -1;
    }

    /* Poll for connection (same approach that worked in diagnostic test) */
    start = clock();
    deadline = start + (clock_t)wait_secs * CLOCKS_PER_SEC;

    {
        int tick_count = 0;
        clock_t last_print = start;

        while (clock() < deadline) {
            if (!tcp_tick(&tcp_sock)) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "Connection refused (tcp_tick failed after %d ticks)",
                         tick_count);
                ctx->state = NET_ERROR;
                return -1;
            }

            tick_count++;

            if (sock_established(&tcp_sock)) {
                ctx->state = NET_CONNECTED;
                ctx->recv_pos = 0;
                ctx->recv_need = HEADER_SIZE;

                /* Enlarge TCP receive buffer for high throughput.
                 * This also increases the TCP window advertised to
                 * the server, letting it send more data at once. */
                sock_setbuf((sock_type *)&tcp_sock, big_rx_buf,
                            BIG_RX_BUF_SIZE);

                printf("Connected after %d ticks!\n", tick_count);
                printf("TCP rx buffer: %d bytes\n", BIG_RX_BUF_SIZE);
                return 0;
            }

            /* Print progress every second */
            if (clock() - last_print >= CLOCKS_PER_SEC) {
                printf("  ...waiting (tick %d)...\n", tick_count);
                last_print = clock();
            }
        }
    }

    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "Connection timed out after %d seconds", wait_secs);
    sock_close(&tcp_sock);
    ctx->state = NET_ERROR;
    return -1;
}

void net_close(net_context_t *ctx)
{
    if (ctx->state == NET_CONNECTED || ctx->state == NET_CONNECTING) {
        sock_close(&tcp_sock);
        /* Give it a moment to send FIN */
        sock_wait_closed(&tcp_sock, 2, NULL, NULL);
    }
sock_err:
    ctx->state = NET_DISCONNECTED;
}

int net_send_message(net_context_t *ctx, uint8_t msg_type,
                     const uint8_t *payload, uint16_t payload_len)
{
    uint8_t header_buf[HEADER_SIZE];
    int sent;

    if (ctx->state != NET_CONNECTED) {
        return -1;
    }

    /* Encode and send header */
    proto_encode_header(header_buf, msg_type, 0, payload_len,
                        proto_next_seq());

    sent = sock_write(&tcp_sock, header_buf, HEADER_SIZE);
    if (sent < HEADER_SIZE) {
        ctx->state = NET_ERROR;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Send header failed");
        return -1;
    }

    /* Send payload if any */
    if (payload_len > 0 && payload != NULL) {
        sent = sock_write(&tcp_sock, payload, payload_len);
        if (sent < payload_len) {
            ctx->state = NET_ERROR;
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "Send payload failed");
            return -1;
        }
    }

    return 0;
}

int net_recv_message(net_context_t *ctx, msg_header_t *header,
                     uint8_t *payload, uint16_t *payload_len)
{
    int avail, to_read, got;

    if (ctx->state != NET_CONNECTED) {
        return -1;
    }

    /* Check if socket is still alive */
    if (!tcp_tick(&tcp_sock)) {
        ctx->state = NET_ERROR;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Connection lost");
        return -1;
    }

    /* Check how much data is available */
    avail = sock_dataready(&tcp_sock);
    if (avail <= 0) {
        return 0; /* no data yet */
    }

    /* Phase 1: Read the header */
    if (ctx->recv_pos < HEADER_SIZE) {
        to_read = HEADER_SIZE - ctx->recv_pos;
        if (to_read > avail) to_read = avail;

        got = sock_fastread(&tcp_sock, ctx->recv_buf + ctx->recv_pos, to_read);
        if (got <= 0) return 0;

        ctx->recv_pos += got;
        if (ctx->recv_pos < HEADER_SIZE) {
            return 0; /* still need more header bytes */
        }

        /* Header complete - decode it to find payload length */
        proto_decode_header(ctx->recv_buf, header);
        ctx->recv_need = HEADER_SIZE + header->payload_len;

        /* If no payload, message is complete */
        if (header->payload_len == 0) {
            *payload_len = 0;
            ctx->recv_pos = 0;
            ctx->recv_need = HEADER_SIZE;
            return 1;
        }

        /* Sanity check */
        if (header->payload_len > MAX_PAYLOAD_SIZE) {
            ctx->state = NET_ERROR;
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "Payload too large: %u", header->payload_len);
            return -1;
        }

        /* Check if more data is available for the payload */
        avail = sock_dataready(&tcp_sock);
        if (avail <= 0) return 0;
    } else {
        /* We already have the header decoded from a previous call */
        proto_decode_header(ctx->recv_buf, header);
    }

    /* Phase 2: Read the payload */
    to_read = ctx->recv_need - ctx->recv_pos;
    if (to_read > avail) to_read = avail;

    got = sock_fastread(&tcp_sock, ctx->recv_buf + ctx->recv_pos, to_read);
    if (got <= 0) return 0;

    ctx->recv_pos += got;

    if (ctx->recv_pos < ctx->recv_need) {
        return 0; /* still need more payload bytes */
    }

    /* Complete message received! */
    *payload_len = header->payload_len;
    memcpy(payload, ctx->recv_buf + HEADER_SIZE, header->payload_len);

    /* Reset for next message */
    ctx->recv_pos = 0;
    ctx->recv_need = HEADER_SIZE;

    return 1;
}

void net_poll(void)
{
    tcp_tick(NULL);
}

int net_data_ready(void)
{
    return sock_dataready(&tcp_sock) > 0 ? 1 : 0;
}

/* --- Non-blocking connect steps --- */

int net_resolve_host(net_context_t *ctx, const char *server_ip)
{
    DWORD ip;

    memset(ctx, 0, sizeof(net_context_t));
    ctx->state = NET_CONNECTING;

    ip = resolve(server_ip);
    if (ip == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Cannot resolve: %s", server_ip);
        ctx->state = NET_ERROR;
        return -1;
    }
    ctx->server_ip = ip;
    return 0;
}

int net_start_connect(net_context_t *ctx, uint16_t port)
{
    ctx->server_port = port;

    if (!tcp_open(&tcp_sock, 0, ctx->server_ip, port, NULL)) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "tcp_open() failed");
        ctx->state = NET_ERROR;
        return -1;
    }
    return 0;
}

int net_poll_connect(net_context_t *ctx)
{
    if (!tcp_tick(&tcp_sock)) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Connection refused");
        ctx->state = NET_ERROR;
        return -1;
    }
    if (sock_established(&tcp_sock)) {
        return 1;  /* Connected! */
    }
    return 0;  /* Still waiting */
}

void net_finish_connect(net_context_t *ctx)
{
    ctx->state = NET_CONNECTED;
    ctx->recv_pos = 0;
    ctx->recv_need = HEADER_SIZE;

    /* Enlarge TCP receive buffer for high throughput */
    sock_setbuf((sock_type *)&tcp_sock, big_rx_buf, BIG_RX_BUF_SIZE);
}
