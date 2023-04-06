/*
 * Copyright © 2013 Amaury Pouly <amaury.pouly@lowrisc.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libusbi.h"
#include "libusb_redir.hpp"
#include "version.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

struct redir_context_priv
{
    pthread_t event_thread;
    /* FIXME protect this by a lock */
    int socket; /* the socket to talk to the server */
};

struct redir_device_priv
{
    /* unique ID for the device (generated by host) */
    uint32_t device_id;
    uint32_t next_transfer_id;
};

struct redir_transfer_priv
{
    /* unique ID for the transfer */
    uint32_t transfer_id;
    /* transfer status */
    enum libusb_transfer_status status;
};

static void init_device_priv(struct redir_device_priv *priv)
{
    priv->device_id = 0;
    priv->next_transfer_id = 19;
}

static int write_or_die(int socket, const void *buf, size_t size)
{
    const char *ptr = buf;
    while(size > 0)
    {
        int cnt = write(socket, ptr, size);
        if(cnt == 0)
            return LIBUSB_ERROR_IO; /* investigate if/when this can happen */
        if(cnt < 0)
            return cnt;
        size -= cnt;
        ptr += cnt;
    }
    return 0;
}

static int read_or_die(int socket, void *buf, size_t size)
{
    char *ptr = buf;
    while(size > 0)
    {
        int cnt = read(socket, ptr, size);
        if(cnt == 0)
            return LIBUSB_ERROR_IO; /* investigate if/when this can happen */
        if(cnt < 0)
            return LIBUSB_ERROR_IO;
        size -= cnt;
        ptr += cnt;
    }
    return 0;
}

/* send packet, the pointer is to the payload, the header is handled by
 * this function; return 0 on success */
static int send_packet(struct libusb_context *ctx,
    enum libusb_redir_packet_type type,
    const void *packet, size_t length)
{
    usbi_dbg(ctx, "sending packet type %lu, length %lu", (unsigned long)type, (unsigned long)length);
    struct redir_context_priv *priv = usbi_get_context_priv(ctx);
    struct libusb_redir_packet_header hdr;
    hdr.type = type;
    hdr.length = length; /* truncate, check length */
    int err = write_or_die(priv->socket, &hdr, sizeof(hdr));
    if(err < 0)
        return err;
    /* handle empty payload */
    if(packet == 0 || length == 0)
        return 0;
    return write_or_die(priv->socket, packet, length);
}

/* Receive packet, allocate buffer. */
static int recv_packet(struct libusb_context *ctx,
    libusb_redir_packet_type_t *out_type,
    void **packet, size_t *out_length)
{
    struct redir_context_priv *priv = usbi_get_context_priv(ctx);
    usbi_dbg(ctx, "wait for packet");
    struct libusb_redir_packet_header hdr;

    int err = read_or_die(priv->socket, &hdr, sizeof(hdr));
    if(err < 0)
        return err;
    usbi_dbg(ctx, "  got packet type %lu length %lu", (unsigned long)hdr.type, (unsigned long)hdr.length);
    /* FIXME prevent attacker controlled allocation properly here */
    void *payload = malloc(hdr.length);
    if(payload == NULL)
        return LIBUSB_ERROR_NO_MEM;
    err = read_or_die(priv->socket, payload, hdr.length);
    if(err < 0)
    {
        free(payload);
        return err;
    }
    usbi_dbg(ctx, "  got packet data");
    *out_type = hdr.type;
    *packet = payload;
    *out_length = hdr.length;
    return LIBUSB_SUCCESS;
}

static int connect_unix_socket(struct libusb_context *ctx, const char *name)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sock == -1)
    {
        usbi_err(ctx, "error: could not create unix socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un sockaddr;
    sockaddr.sun_family = AF_UNIX;
    if(strlen(name) + 1 > sizeof(sockaddr.sun_path))
    {
        usbi_err(ctx, "error: unix socket name is too long");
        close(sock);
        return -1;
    }
    strcpy(sockaddr.sun_path, name);
    /* if the first character of name is @, create an abstract socket */
    if(name[0] == '@')
        sockaddr.sun_path[0] = 0; /* creates an abstract socket */
    int err = connect(sock, (struct sockaddr*)&sockaddr, sizeof(sockaddr.sun_family) + strlen(name));
    if(err != 0)
    {
        usbi_err(ctx, "error: could not connect socket: %s", strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

static int redir_send_hello(struct libusb_context *ctx)
{
    usbi_dbg(ctx, "send hello");
    libusb_redir_hello_packet_t hello =
    {
        .magic = LIBUSB_REDIR_HELLO_MAGIC,
        .protocol_version = LIBUSB_REDIR_V1,
        .impl_version = {0},
    };
    snprintf(hello.impl_version, sizeof(hello.impl_version),
             "libusb %d.%d.%d.%d.%s", LIBUSB_MAJOR, LIBUSB_MINOR, LIBUSB_MICRO,
             LIBUSB_NANO, LIBUSB_RC);
    return send_packet(ctx, LIBUSB_REDIR_HELLO, &hello, sizeof(hello));
}

#define CHECK_DBG(ctx, cond, err_code, msg, ...) \
    if(!(cond)) \
    { \
        usbi_dbg(ctx, msg, __VA_ARGS__); \
        return err_code; \
    }

static int do_hello(libusb_context *ctx, libusb_redir_hello_packet_t *in_hello)
{
    /* if the magic and protocol values don't match, don't bothering answering */
    CHECK_DBG(ctx, in_hello->magic == LIBUSB_REDIR_HELLO_MAGIC, LIBUSB_ERROR_NOT_SUPPORTED,
        "magic value is wrong (%llx), expected %llx", (unsigned long long)in_hello->magic,
        (unsigned long long)LIBUSB_REDIR_HELLO_MAGIC);
    CHECK_DBG(ctx, in_hello->protocol_version == LIBUSB_REDIR_V1, LIBUSB_ERROR_NOT_SUPPORTED,
        "protocol value is wrong (%x), expected %x", in_hello->protocol_version,
        LIBUSB_REDIR_V1);
    usbi_dbg(ctx, "received hello, impl_version = %.64s", in_hello->impl_version);
    return LIBUSB_SUCCESS;
}

static void *redir_event_thread_main(void *_ctx)
{
    libusb_context *ctx = _ctx;
    struct redir_context_priv *priv = usbi_get_context_priv(ctx);
    usbi_dbg(ctx, "event thread started");
    bool stop = false;
    while(!stop)
    {
        libusb_redir_packet_type_t type;
        void *payload = NULL;
        size_t length;
        /* recv is a cancellation point for pthread */
        int err = recv_packet(ctx, &type, &payload, &length);
        if(err < 0)
            break;
        #define DO_PAYLOAD_SIZE_EXACT(pkt_type_str, type, fn) \
            do { \
                if(length != sizeof(type)) { \
                    usbi_dbg(ctx, "%s packet has wrong payload size %lu (expected %lu), ignore", \
                        pkt_type_str, (unsigned long)length, (unsigned long)sizeof(type)); \
                } \
                else { \
                    err = fn(ctx, payload); \
                    if(err < 0) { \
                        usbi_dbg(ctx, "fatal error when handling %s packet: err=%d", pkt_type_str, err); \
                        stop = true; \
                    } \
                } \
                free(payload); \
            } while(0)

        switch(type)
        {
            case LIBUSB_REDIR_HELLO:
                DO_PAYLOAD_SIZE_EXACT("hello", libusb_redir_hello_packet_t, do_hello);
                break;
            default:
                /* ignore */
                usbi_dbg(ctx, "ignore request %lu", (unsigned long)type);
                break;
        }
    }
    return NULL;
}

static int redir_stop_event_thread(struct libusb_context *ctx)
{
    struct redir_context_priv *priv = usbi_get_context_priv(ctx);
    usbi_dbg(ctx, "cancelling thread");
    int err = pthread_cancel(priv->event_thread);
    if(err != 0)
    {
        usbi_dbg(ctx, "cannot stop thread, this is bad");
        return LIBUSB_ERROR_OTHER;
    }
    usbi_dbg(ctx, "waiting for thread to stop");
    err = pthread_join(priv->event_thread, NULL);
    if(err == 0)
        usbi_dbg(ctx, "cannot wait for thread, this is bad");
    else
        return LIBUSB_ERROR_OTHER;
    usbi_dbg(ctx, "thread has stopped");
    return LIBUSB_SUCCESS;
}

static int redir_init(struct libusb_context *ctx)
{
    struct redir_context_priv *priv = usbi_get_context_priv(ctx);
    usbi_dbg(ctx, "init redir");
    int err = pthread_create(&priv->event_thread, NULL, redir_event_thread_main, ctx);
    if(err != 0)
    {
        usbi_err(ctx, "failed to create hotplug event thread (%d)", err);
        return LIBUSB_ERROR_OTHER;
    }
    priv->socket = connect_unix_socket(ctx, "@libusb_redir");
    if(priv->socket == -1)
    {
        redir_stop_event_thread(ctx);
        return LIBUSB_ERROR_NOT_FOUND;
    }
    usbi_dbg(ctx, "  socket: %d", priv->socket);
    /* send hello */
    int res = redir_send_hello(ctx);
    if(res != LIBUSB_SUCCESS)
    {
        redir_stop_event_thread(ctx);
        return res;
    }
    /* sending the hello will trigger the initial device discovery: the target
     * will start sending a list of devices, followed by LIBUSB_REDIR_INITIAL_DISCOVERY_DONE */
    /* add an event source to poll the socket */
    return usbi_add_event_source(ctx, priv->socket, POLLIN);
}

static void redir_exit(struct libusb_context *ctx)
{
    redir_stop_event_thread(ctx);
}

static void redir_hotplug_poll(struct libusb_context *ctx)
{
    while(1) {}
}

extern "C" {
const struct usbi_os_backend usbi_backend = {
    .name = "Redirect backend",
    .caps = 0,
    .init = redir_init,
    .exit = redir_exit,
    .hotplug_poll = redir_hotplug_poll,

    .context_priv_size = sizeof(struct redir_context_priv),
    .device_priv_size = sizeof(struct redir_device_priv),
    .transfer_priv_size = sizeof(struct redir_transfer_priv)
};
}
