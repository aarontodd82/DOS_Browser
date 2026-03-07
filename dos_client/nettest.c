/*
 * Simple network test for RetroSurf - tests packet driver and TCP/IP
 * Usage: NETTEST.EXE [server_ip]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <tcp.h>

int main(int argc, char *argv[])
{
    const char *server_ip = "192.168.4.45";
    DWORD ip;
    tcp_Socket sock;
    int i;

    if (argc > 1) server_ip = argv[1];

    printf("\n=== RetroSurf Network Test ===\n\n");

    /* Step 1: Initialize Watt-32 (finds packet driver, reads WATTCP.CFG) */
    printf("Step 1: Initializing TCP/IP stack...\n");
    if (sock_init() != 0) {
        printf("  FAILED! Packet driver not found or WATTCP.CFG error.\n");
        printf("\n  Make sure you loaded the packet driver first:\n");
        printf("    NE2000.COM 0x60 10 0x300\n");
        printf("  And that WATTCP.CFG is in the current directory.\n");
        printf("\nPress any key to exit.\n");
        getch();
        return 1;
    }
    printf("  OK! TCP/IP stack initialized.\n");
    printf("  My IP: %s\n", _inet_ntoa(NULL, htonl(my_ip_addr)));

    /* Step 2: Resolve server IP */
    printf("\nStep 2: Resolving server %s...\n", server_ip);
    ip = resolve(server_ip);
    if (ip == 0) {
        printf("  FAILED! Cannot resolve %s\n", server_ip);
        printf("\nPress any key to exit.\n");
        getch();
        sock_exit();
        return 1;
    }
    printf("  OK! Server IP resolved.\n");

    /* Step 3: Try TCP connection */
    printf("\nStep 3: Connecting to %s port 8086...\n", server_ip);
    if (!tcp_open(&sock, 0, ip, 8086, NULL)) {
        printf("  FAILED! tcp_open() error.\n");
        printf("\nPress any key to exit.\n");
        getch();
        sock_exit();
        return 1;
    }

    printf("  Waiting for connection (15 seconds)...\n");
    for (i = 0; i < 150; i++) {
        if (!tcp_tick(&sock)) {
            printf("  FAILED! Connection refused.\n");
            printf("  Server may not be running on %s:8086\n", server_ip);
            printf("\nPress any key to exit.\n");
            getch();
            sock_exit();
            return 1;
        }
        if (sock_established(&sock)) {
            printf("  OK! Connected to server!\n");
            sock_close(&sock);
            printf("\n=== ALL TESTS PASSED ===\n");
            printf("Network is working. You can run RETRO.EXE now.\n");
            printf("\nPress any key to exit.\n");
            getch();
            sock_exit();
            return 0;
        }
        delay(100);

        /* Print progress every 3 seconds */
        if (i > 0 && i % 30 == 0) {
            printf("  ...still waiting (%d seconds)...\n", i / 10);
        }
    }

    printf("  FAILED! Connection timed out.\n");
    printf("  Check that server.py is running on %s\n", server_ip);
    printf("  and that firewall allows port 8086.\n");
    sock_close(&sock);
    printf("\nPress any key to exit.\n");
    getch();
    sock_exit();
    return 1;
}
