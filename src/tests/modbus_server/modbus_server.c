/*
 * Copyright © Stéphane Raimbault <stephane.raimbault@gmail.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * KRH: This server is directly cloned from the main GitHub libmodbus repo.
 *
 * Only minor changes were done to quiet clang-tidy and change the default port.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <modbus.h>

#if defined(_WIN32)
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    ifdef HAVE_NETINET_IN_H
#        include <netinet/in.h>
#    endif /* HAVE_NETINET_IN_H */
#    include <sys/select.h>
#    include <sys/socket.h>
#endif

#define NB_CONNECTION 5

static modbus_t *ctx = NULL;
static modbus_mapping_t *mb_mapping;

static int server_socket = -1;

static void close_sigint(int dummy) {
    if(server_socket != -1) { close(server_socket); }
    modbus_free(ctx);
    modbus_mapping_free(mb_mapping);

    exit(dummy);
}

int main(void) {
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
    int master_socket;
    int rc;
    fd_set refset;
    fd_set rdset;
    /* Maximum file descriptor number */
    int fdmax;

    ctx = modbus_new_tcp("127.0.0.1", 5020);

    mb_mapping =
        modbus_mapping_new(MODBUS_MAX_READ_BITS, MODBUS_MAX_READ_BITS, MODBUS_MAX_READ_REGISTERS, MODBUS_MAX_READ_REGISTERS);
    if(mb_mapping == NULL) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Failed to allocate the mapping: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    server_socket = modbus_tcp_listen(ctx, NB_CONNECTION);
    if(server_socket == -1) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to listen TCP connection\n");
        modbus_free(ctx);
        return -1;
    }

    signal(SIGINT, close_sigint);

    /* Clear the reference set of socket */
    FD_ZERO(&refset);
    /* Add the server socket */
    FD_SET(server_socket, &refset);

    /* Keep track of the max file descriptor */
    fdmax = server_socket;

    for(;;) {
        rdset = refset;
        if(select(fdmax + 1, &rdset, NULL, NULL, NULL) == -1) {
            perror("Server select() failure.");
            close_sigint(1);
        }

        /* Run through the existing connections looking for data to be
         * read */
        for(master_socket = 0; master_socket <= fdmax; master_socket++) {

            if(!FD_ISSET(master_socket, &rdset)) { continue; }

            if(master_socket == server_socket) {
                /* A client is asking a new connection */
                socklen_t addrlen;
                struct sockaddr_in clientaddr;
                int newfd;

                /* Handle new connections */
                addrlen = sizeof(clientaddr);
                // NOLINTNEXTLINE
                memset(&clientaddr, 0, sizeof(clientaddr));
                newfd = accept(server_socket, (struct sockaddr *)&clientaddr, &addrlen);
                if(newfd == -1) {
                    perror("Server accept() error");
                } else {
                    FD_SET(newfd, &refset);

                    if(newfd > fdmax) {
                        /* Keep track of the maximum */
                        fdmax = newfd;
                    }
                    printf("New connection from %s:%d on socket %d\n", inet_ntoa(clientaddr.sin_addr), clientaddr.sin_port,
                           newfd);
                }
            } else {
                modbus_set_socket(ctx, master_socket);
                rc = modbus_receive(ctx, query);
                if(rc > 0) {
                    modbus_reply(ctx, query, rc, mb_mapping);
                } else if(rc == -1) {
                    /* This example server in ended on connection closing or
                     * any errors. */
                    printf("Connection closed on socket %d\n", master_socket);
                    close(master_socket);

                    /* Remove from reference set */
                    FD_CLR(master_socket, &refset);

                    if(master_socket == fdmax) { fdmax--; }
                }
            }
        }
    }

    return 0;
}
