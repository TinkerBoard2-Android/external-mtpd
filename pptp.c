/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* A simple implementation of PPTP Network Server (RFC 2637) which only
 * creates a single session. The following code only handles control packets.
 * Data packets are handled by PPPoPNS driver which can be found in Android
 * kernel tree. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/netdevice.h>
#include <linux/if_pppox.h>
#include <linux/types.h>

#include "mtpd.h"

enum pptp_message {
    SCCRQ = 1,
    SCCRP = 2,
    STOPCCRQ = 3,
    STOPCCRP = 4,
    ECHORQ = 5,
    ECHORP = 6,
    OCRQ = 7,
    OCRP = 8,
    ICRQ = 9,
    ICRP = 10,
    ICCN = 11,
    CCRQ = 12,
    CDN = 13,
    WEN = 14,
    SLI = 15,
    MESSAGE_MAX = 15,
};

static char *messages[] = {
    NULL, "SCCRQ", "SCCRP", "STOPCCRQ", "STOPCCRP", "ECHORQ", "ECHORP",
    "OCRQ", "OCRP", "ICRQ", "ICRP", "ICCN", "CCRQ", "CDN", "WEN", "SLI",
};

static uint8_t lengths[] = {
    0, 156, 156, 16, 16, 16, 20, 168, 32, 220, 24, 28, 16, 148, 40, 24,
};

#define CONTROL_MESSAGE         htons(1)
#define MAGIC_COOKIE            htonl(0x1A2B3C4D)
#define PROTOCOL_VERSION        htons(0x0100)

#define RESULT_OK               1
#define RESULT_ERROR            2

/* Some implementation uses 0 instead of 1, so we allow both of them. */
#define ESTABLISHED(result)     (result <= 1)

#define HEADER_SIZE             8
#define MIN_MESSAGE_SIZE        10

static __be16 local;
static __be16 remote;
static uint16_t state;
static const char *remote_name;    /* server host name or IP address */

#define MAX_PACKET_LENGTH       220

/* We define all the fields we used in this structure. Type conversion and byte
 * alignment are solved in one place. Although it looks a little bit ugly, it
 * really makes life easier. */
static struct packet {
    int length;
    int expect;
    union {
        uint8_t buffer[MAX_PACKET_LENGTH];
        struct {
            struct __attribute__((packed)) {
                uint16_t length;
                uint16_t type;
                uint32_t cookie;
            } header;
            uint16_t message;
            uint16_t reserved;
            union {
                struct __attribute__((packed)) {
                    uint16_t protocol_version;
                    uint8_t result;
                    uint8_t error;
                    uint32_t framing;
                    uint32_t bearer;
                    uint16_t channels;
                    uint16_t firmware_revision;
                    char host[64];
                } sccrp, sccrq;
                struct __attribute__((packed)) {
                    uint16_t call;
                    uint16_t serial;
                    uint32_t minimum_speed;
                    uint32_t maximum_speed;
                    uint32_t bearer;
                    uint32_t framing;
                    uint16_t window_size;
                } ocrq;
                struct __attribute__((packed)) {
                    uint16_t call;
                    uint16_t peer;
                    uint8_t result;
                } ocrp, icrp;
                struct __attribute__((packed)) {
                    uint32_t identifier;
                    uint8_t result;
                } echorq, echorp;
                struct __attribute__((packed)) {
                    uint16_t call;
                } icrq, ccrq, cdn;
            };
        } __attribute__((packed));
    } __attribute__((aligned(4)));
} incoming, outgoing;

static void set_message(uint16_t message)
{
    uint16_t length = lengths[message];
    memset(outgoing.buffer, 0, length);
    outgoing.length = length;
    outgoing.header.length = htons(length);
    outgoing.header.type = CONTROL_MESSAGE;
    outgoing.header.cookie = MAGIC_COOKIE;
    outgoing.message = htons(message);
}

static void send_packet()
{
    send(the_socket, outgoing.buffer, outgoing.length, 0);
}

static int recv_packet()
{
    int length;

    /* We are going to read a new message if incoming.expect is 0. */
    if (!incoming.expect) {
        incoming.length = 0;
        incoming.expect = HEADER_SIZE;
    }

    /* The longest message defined in RFC 2637 is 220 bytes, but the protocol
     * itself allows up to 65536 bytes. Therefore we always read a complete
     * message but only keep the first 220 bytes before passing up. */
    length = incoming.expect - incoming.length;
    if (incoming.length >= MAX_PACKET_LENGTH) {
        uint8_t buffer[length];
        length = recv(the_socket, buffer, length, 0);
    } else {
        if (incoming.expect > MAX_PACKET_LENGTH) {
            length = MAX_PACKET_LENGTH - incoming.length;
        }
        length = recv(the_socket, &incoming.buffer[incoming.length], length, 0);
    }
    if (length == -1) {
        if (errno == EINTR) {
            return 0;
        }
        log_print(FATAL, "Recv() %s", strerror(errno));
        exit(NETWORK_ERROR);
    }
    if (length == 0) {
        log_print(DEBUG, "Connection closed");
        log_print(INFO, "Remote server hung up");
        return -REMOTE_REQUESTED;
    }
    incoming.length += length;

    /* If incoming.header is valid, check cookie and update incoming.expect. */
    if (incoming.length == HEADER_SIZE && incoming.expect == HEADER_SIZE) {
        if (incoming.header.cookie != MAGIC_COOKIE) {
            log_print(DEBUG, "Loss of synchronization");
            log_print(ERROR, "Protocol error");
            return -PROTOCOL_ERROR;
        }
        incoming.expect = ntohs(incoming.header.length);
        if (incoming.expect < HEADER_SIZE) {
            log_print(DEBUG, "Invalid message length");
            log_print(ERROR, "Protocol error");
            return -PROTOCOL_ERROR;
        }
    }

    /* Now we have a complete message. Reset incoming.expect. */
    if (incoming.length == incoming.expect) {
        incoming.expect = 0;

        /* Return 1 if it is a control message. */
        if (incoming.header.type == CONTROL_MESSAGE) {
            return 1;
        }
        log_print(DEBUG, "Ignored non-control message (type = %u)",
                (unsigned)ntohs(incoming.header.type));
    }
    return 0;
}

static int pptp_connect(char **arguments)
{
    remote_name = arguments[0];

    create_socket(AF_UNSPEC, SOCK_STREAM, arguments[0], arguments[1]);

    log_print(DEBUG, "Sending SCCRQ");
    state = SCCRQ;
    set_message(SCCRQ);
    outgoing.sccrq.protocol_version = PROTOCOL_VERSION;
    outgoing.sccrq.framing = htonl(3);
    outgoing.sccrq.bearer = htonl(3);
    outgoing.sccrq.channels = htons(1);
    strcpy(outgoing.sccrq.host, "anonymous");
    send_packet();
    return 0;
}

/**
 * Check if upstream kernel implementation of PPTP should be used.
 *
 * @return true If upstream PPTP should be used, which is the case if
 *              the obsolete OPNS feature is not available.
 */
static bool check_pptp(void)
{
    int fd = socket(AF_PPPOX, SOCK_DGRAM, PX_PROTO_OPNS);

    if (fd < 0) {
        return true;
    } else {
        close(fd);
        return false;
    }
}

/**
 * Create OPNS session.
 *
 * @deprecated It will be removed soon in favor of upstream PPTP.
 *
 * @return PPPoX socket file descriptor
 */
static int create_pppox_opns(void)
{
    int pppox;

    log_print(WARNING, "Using deprecated OPNS protocol. "
                       "Its support will be removed soon. "
                       "Please enable PPTP support in your kernel");

    log_print(INFO, "Creating PPPoX socket");
    pppox = socket(AF_PPPOX, SOCK_DGRAM, PX_PROTO_OPNS);

    if (pppox == -1) {
        log_print(FATAL, "Socket() %s", strerror(errno));
        exit(SYSTEM_ERROR);
    } else {
        struct sockaddr_pppopns address = {
            .sa_family = AF_PPPOX,
            .sa_protocol = PX_PROTO_OPNS,
            .tcp_socket = the_socket,
            .local = local,
            .remote = remote,
        };
        if (connect(pppox, (struct sockaddr *)&address, sizeof(address))) {
            log_print(FATAL, "Connect() %s", strerror(errno));
            exit(SYSTEM_ERROR);
        }
    }
    return pppox;
}

/**
 * Get IP address by host name.
 *
 * @param name Host name to get IP address for
 *
 * @return IP address for given host name
 */
static struct in_addr get_addr_by_name(const char *name)
{
    struct addrinfo hints;
    struct addrinfo *res, *rp;
    struct in_addr addr;
    int err;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;       /* allow only IPv4 */
    hints.ai_socktype = SOCK_DGRAM;  /* UDP */
    hints.ai_protocol = 0;           /* any protocol */

    err = getaddrinfo(name, NULL, &hints, &res);
    if (err) {
        log_print(FATAL, "%s: getaddrinfo: %s", __func__, gai_strerror(err));
        exit(SYSTEM_ERROR);
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        /* For now we only support IPv4 */
        if (rp->ai_family == AF_INET) {
            addr = ((struct sockaddr_in *)rp->ai_addr)->sin_addr;
            break;
        }
    }

    if (rp == NULL) {
        log_print(FATAL, "%s: No IPv4 addresses found", __func__);
        freeaddrinfo(res);
        exit(SYSTEM_ERROR);
    }

    freeaddrinfo(res);

    return addr;
}

/**
 * Get local IP address.
 *
 * Make a socket connection with remote server and then call getsockname() on
 * the connected socket. This will return the local IP address.
 *
 * @param remote_addr Server IP address
 *
 * @return Local IP address
 */
static struct in_addr get_local_addr(struct in_addr remote_addr)
{
    int sock;
    struct sockaddr_in addr;
    socklen_t addr_len;

    addr_len = sizeof(struct sockaddr_in);
    addr.sin_addr = remote_addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        log_print(FATAL, "%s: Socket() %s", __func__, strerror(errno));
        exit(SYSTEM_ERROR);
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr))) {
        close(sock);
        log_print(FATAL, "%s: Connect() %s", __func__, strerror(errno));
        exit(SYSTEM_ERROR);
    }

    getsockname(sock, (struct sockaddr*)&addr, &addr_len);
    close(sock);

    return addr.sin_addr;
}

/**
 * Create PPTP session.
 *
 * @return PPTP socket file descriptor
 */
static int create_pppox_pptp(void)
{
    int pptp_fd;
    struct sockaddr_pppox src, dst;
    struct in_addr remote_addr; /* server IP address */
    struct in_addr local_addr;  /* client IP address */

    remote_addr = get_addr_by_name(remote_name);
    local_addr = get_local_addr(remote_addr);

    src.sa_family = AF_PPPOX;
    src.sa_protocol = PX_PROTO_PPTP;
    src.sa_addr.pptp.call_id = ntohs(local);
    src.sa_addr.pptp.sin_addr = local_addr;

    dst.sa_family = AF_PPPOX;
    dst.sa_protocol = PX_PROTO_PPTP;
    dst.sa_addr.pptp.call_id = ntohs(remote);
    dst.sa_addr.pptp.sin_addr = remote_addr;

    pptp_fd = socket(AF_PPPOX, SOCK_STREAM, PX_PROTO_PPTP);
    if (pptp_fd < 0) {
        log_print(FATAL, "Failed to create PPTP socket (%s)", strerror(errno));
        exit(SYSTEM_ERROR);
    }

    if (bind(pptp_fd, (struct sockaddr*)&src, sizeof(src))) {
        log_print(FATAL, "Failed to bind PPTP socket (%s)", strerror(errno));
        close(pptp_fd);
        exit(SYSTEM_ERROR);
    }

    if (connect(pptp_fd, (struct sockaddr*)&dst, sizeof(dst))) {
        log_print(FATAL, "Failed to connect PPTP socket (%s)", strerror(errno));
        close(pptp_fd);
        exit(SYSTEM_ERROR);
    }

    return pptp_fd;
}

static int pptp_process()
{
    int result = recv_packet();
    if (result <= 0) {
        return result;
    }

    if (incoming.length < MIN_MESSAGE_SIZE) {
        log_print(DEBUG, "Control message too short");
        return 0;
    }
    incoming.message = ntohs(incoming.message);
    if (incoming.message > MESSAGE_MAX || !messages[incoming.message]) {
        log_print(DEBUG, "Received UNKNOWN %d", incoming.message);
        return 0;
    }
    if (incoming.length < lengths[incoming.message]) {
        log_print(DEBUG, "Received %s with invalid length (length = %d)",
                messages[incoming.message], incoming.length);
        return 0;
    }

    switch(incoming.message) {
        case SCCRP:
            if (state == SCCRQ) {
                if (incoming.sccrp.protocol_version == PROTOCOL_VERSION &&
                        ESTABLISHED(incoming.sccrp.result)) {
                    while (!local) {
                        local = random();
                    }
                    log_print(DEBUG, "Received SCCRP -> Sending OCRQ "
                            "(local = %u)", (unsigned)ntohs(local));
                    log_print(INFO, "Tunnel established");
                    state = OCRQ;
                    set_message(OCRQ);
                    outgoing.ocrq.call = local;
                    outgoing.ocrq.serial = random();
                    outgoing.ocrq.minimum_speed = htonl(1000);
                    outgoing.ocrq.maximum_speed = htonl(100000000);
                    outgoing.ocrq.bearer = htonl(3);
                    outgoing.ocrq.framing = htonl(3);
                    outgoing.ocrq.window_size = htons(8192);
                    send_packet();
                    return 0;
                }
                log_print(DEBUG, "Received SCCRP (result = %d)",
                        incoming.sccrq.result);
                log_print(INFO, "Remote server hung up");
                return -REMOTE_REQUESTED;
            }
            break;

        case OCRP:
            if (state == OCRQ && incoming.ocrp.peer == local) {
                if (ESTABLISHED(incoming.ocrp.result)) {
                    remote = incoming.ocrp.call;
                    log_print(DEBUG, "Received OCRQ (remote = %u)",
                            (unsigned)ntohs(remote));
                    log_print(INFO, "Session established");
                    state = OCRP;

                    if (check_pptp())
                        start_pppd_pptp(create_pppox_pptp());
                    else
                        start_pppd(create_pppox_opns());

                    return 0;
                }
                log_print(DEBUG, "Received OCRP (result = %d)",
                        incoming.ocrp.result);
                log_print(INFO, "Remote server hung up");
                return -REMOTE_REQUESTED;
            }
            break;

        case STOPCCRQ:
            log_print(DEBUG, "Received STOPCCRQ");
            log_print(INFO, "Remote server hung up");
            state = STOPCCRQ;
            return -REMOTE_REQUESTED;

        case CCRQ:
            /* According to RFC 2637 page 45, we should never receive CCRQ for
             * outgoing calls. However, some implementation only acts as PNS and
             * always uses CCRQ to clear a call, so here we still handle it. */
            if (state == OCRP && incoming.ccrq.call == remote) {
                log_print(DEBUG, "Received CCRQ (remote = %u)",
                        (unsigned)ntohs(remote));
                log_print(INFO, "Remote server hung up");
                return -REMOTE_REQUESTED;
            }
            break;

        case CDN:
            if (state == OCRP && incoming.cdn.call == remote) {
                log_print(DEBUG, "Received CDN (remote = %u)",
                        (unsigned)ntohs(remote));
                log_print(INFO, "Remote server hung up");
                return -REMOTE_REQUESTED;
            }
            break;

        case ECHORQ:
            log_print(DEBUG, "Received ECHORQ -> Sending ECHORP");
            set_message(ECHORP);
            outgoing.echorp.identifier = incoming.echorq.identifier;
            outgoing.echorp.result = RESULT_OK;
            send_packet();
            return 0;

        case WEN:
        case SLI:
            log_print(DEBUG, "Recevied %s", messages[incoming.message]);
            return 0;

        case ICRQ:
            log_print(DEBUG, "Received ICRQ (remote = %u, call = %u) -> "
                    "Sending ICRP with error", (unsigned)ntohs(remote),
                    (unsigned)ntohs(incoming.icrq.call));
            set_message(ICRP);
            outgoing.icrp.peer = incoming.icrq.call;
            outgoing.icrp.result = RESULT_ERROR;
            send_packet();
            return 0;

        case OCRQ:
            log_print(DEBUG, "Received OCRQ (remote = %u, call = %u) -> "
                    "Sending OCRP with error", (unsigned)ntohs(remote),
                    (unsigned)ntohs(incoming.ocrq.call));
            set_message(OCRP);
            outgoing.ocrp.peer = incoming.ocrq.call;
            outgoing.ocrp.result = RESULT_ERROR;
            send_packet();
            return 0;
    }

    /* We reach here if we got an unexpected message. Just log it. */
    log_print(DEBUG, "Received UNEXPECTED %s", messages[incoming.message]);
    return 0;
}

static int pptp_timeout()
{
    return 0;
}

static void pptp_shutdown()
{
    /* Normally we should send STOPCCRQ and wait for STOPCCRP, but this might
     * block for a long time. Here we simply take the shortcut: do nothing. */
}

struct protocol pptp = {
    .name = "pptp",
    .arguments = 2,
    .usage = "<server> <port>",
    .connect = pptp_connect,
    .process = pptp_process,
    .timeout = pptp_timeout,
    .shutdown = pptp_shutdown,
};
