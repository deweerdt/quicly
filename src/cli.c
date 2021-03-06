/*
 * Copyright (c) 2017 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 /* required for glibc to use getaddrinfo, etc. */
#endif

#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include "quicly.h"
#include "../deps/picotls/t/util.h"

static unsigned verbosity = 0;

static void hexdump(const char *title, const uint8_t *p, size_t l)
{
    fprintf(stderr, "%s (%zu bytes):\n", title, l);

    while (l != 0) {
        int i;
        fputs("   ", stderr);
        for (i = 0; i < 16; ++i) {
            fprintf(stderr, " %02x", *p++);
            if (--l == 0)
                break;
        }
        fputc('\n', stderr);
    }
}

static int on_stream_open(quicly_stream_t *stream);

static ptls_context_t tlsctx = {ptls_openssl_random_bytes, ptls_openssl_key_exchanges, ptls_openssl_cipher_suites};
static quicly_context_t ctx = {&tlsctx,
                               1280,
                               1000,
                               {16384, 65536, 200, 600},
                               {0},
                               quicly_default_alloc_packet,
                               quicly_default_free_packet,
                               quicly_default_alloc_stream,
                               quicly_default_free_stream,
                               on_stream_open,
                               quicly_default_now};

static void send_data(quicly_stream_t *stream, const char *s)
{
    quicly_sendbuf_write(&stream->sendbuf, s, strlen(s), NULL);
    quicly_sendbuf_shutdown(&stream->sendbuf);
}

static int on_req_receive(quicly_stream_t *stream)
{
    ptls_iovec_t input;

    if (stream->recvbuf.data_off == 0) {
        const char *s = "Hello world!\nThe request was: ";
        quicly_sendbuf_write(&stream->sendbuf, s, strlen(s), NULL);
    }
    while ((input = quicly_recvbuf_get(&stream->recvbuf)).len != 0) {
        quicly_sendbuf_write(&stream->sendbuf, input.base, input.len, NULL);
        quicly_recvbuf_shift(&stream->recvbuf, input.len);
    }
    if (quicly_recvbuf_is_shutdown(&stream->recvbuf))
        quicly_sendbuf_shutdown(&stream->sendbuf);

    return 0;
}

static int on_resp_receive(quicly_stream_t *stream)
{
    ptls_iovec_t input;

    while ((input = quicly_recvbuf_get(&stream->recvbuf)).len != 0) {
        fwrite(input.base, 1, input.len, stdout);
        quicly_recvbuf_shift(&stream->recvbuf, input.len);
    }

    if (quicly_recvbuf_is_shutdown(&stream->recvbuf))
        exit(0);

    return 0;
}

int on_stream_open(quicly_stream_t *stream)
{
    stream->on_update = on_req_receive;
    return 0;
}

static int send_pending(int fd, quicly_conn_t *conn)
{
    quicly_raw_packet_t *packets[16];
    size_t num_packets, i;
    int ret;

    do {
        num_packets = sizeof(packets) / sizeof(packets[0]);
        ret = quicly_send(conn, packets, &num_packets);

        for (i = 0; i != num_packets; ++i) {
            struct msghdr mess;
            struct iovec vec;
            memset(&mess, 0, sizeof(mess));
            mess.msg_name = &packets[i]->sa;
            mess.msg_namelen = packets[i]->salen;
            vec.iov_base = packets[i]->data.base;
            vec.iov_len = packets[i]->data.len;
            mess.msg_iov = &vec;
            mess.msg_iovlen = 1;
            if (verbosity >= 2)
                hexdump("sendmsg", vec.iov_base, vec.iov_len);
            while ((ret = (int)sendmsg(fd, &mess, 0)) == -1 && errno == EINTR)
                ;
            if (ret == -1)
                perror("sendmsg failed");
            ret = 0;
            quicly_default_free_packet(&ctx, packets[i]);
        }
    } while (ret == 0 && num_packets != 0);

    return ret;
}

static int run_client(struct sockaddr *sa, socklen_t salen, const char *host)
{
    int fd, ret;
    struct sockaddr_in local;
    quicly_conn_t *conn = NULL;

    if ((fd = socket(sa->sa_family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        perror("socket(2) failed");
        return 1;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    if (bind(fd, (void *)&local, sizeof(local)) != 0) {
        perror("bind(2) failed");
        return 1;
    }
    ret = quicly_connect(&conn, &ctx, host, sa, salen, NULL);
    assert(ret == 0);
    send_pending(fd, conn);

    while (1) {
        fd_set readfds;
        struct timeval *tv, tvbuf;
        do {
            int64_t timeout_at = conn != NULL ? quicly_get_first_timeout(conn) : INT64_MAX;
            if (timeout_at != INT64_MAX) {
                int64_t delta = timeout_at - quicly_get_context(conn)->now(quicly_get_context(conn));
                if (delta > 0) {
                    tvbuf.tv_sec = delta / 1000;
                    tvbuf.tv_usec = (delta % 1000) * 1000;
                } else {
                    tvbuf.tv_sec = 0;
                    tvbuf.tv_usec = 0;
                }
                tv = &tvbuf;
            } else {
                tv = NULL;
            }
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
        } while (select(fd + 1, &readfds, NULL, NULL, tv) == -1 && errno == EINTR);
        if (FD_ISSET(fd, &readfds)) {
            uint8_t buf[4096];
            struct msghdr mess;
            struct sockaddr sa;
            struct iovec vec;
            memset(&mess, 0, sizeof(mess));
            mess.msg_name = &sa;
            mess.msg_namelen = sizeof(sa);
            vec.iov_base = buf;
            vec.iov_len = sizeof(buf);
            mess.msg_iov = &vec;
            mess.msg_iovlen = 1;
            ssize_t rret;
            while ((rret = recvmsg(fd, &mess, 0)) <= 0)
                ;
            if (verbosity >= 2)
                hexdump("recvmsg", buf, rret);
            quicly_decoded_packet_t packet;
            if (quicly_decode_packet(&packet, buf, rret) == 0) {
                quicly_receive(conn, &packet);
                if (quicly_get_state(conn) == QUICLY_STATE_1RTT_ENCRYPTED && quicly_get_next_stream_id(conn) == 1) {
                    quicly_stream_t *stream;
                    ret = quicly_open_stream(conn, &stream);
                    assert(ret == 0);
                    stream->on_update = on_resp_receive;
                    send_data(stream, "GET /\r\n");
                }
            }
        }
        if (conn != NULL && send_pending(fd, conn) != 0) {
            quicly_free(conn);
            conn = NULL;
        }
    }
}

static int run_server(struct sockaddr *sa, socklen_t salen)
{
    static quicly_conn_t **conns;
    size_t num_conns = 0;
    int fd, ret;

    if ((fd = socket(sa->sa_family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        perror("socket(2) failed");
        return 1;
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        return 1;
    }
    if (bind(fd, sa, salen) != 0) {
        perror("bind(2) failed");
        return 1;
    }

    while (1) {
        fd_set readfds;
        struct timeval *tv, tvbuf;
        do {
            int64_t timeout_at = INT64_MAX;
            size_t i;
            for (i = 0; i != num_conns; ++i) {
                int64_t conn_to = quicly_get_first_timeout(conns[i]);
                if (conn_to < timeout_at)
                    timeout_at = conn_to;
            }
            if (timeout_at != INT64_MAX) {
                int64_t delta = timeout_at - ctx.now(&ctx);
                if (delta > 0) {
                    tvbuf.tv_sec = delta / 1000;
                    tvbuf.tv_usec = (delta % 1000) * 1000;
                } else {
                    tvbuf.tv_sec = 0;
                    tvbuf.tv_usec = 0;
                }
                tv = &tvbuf;
            } else {
                tv = NULL;
            }
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
        } while (select(fd + 1, &readfds, NULL, NULL, tv) == -1 && errno == EINTR);
        if (FD_ISSET(fd, &readfds)) {
            uint8_t buf[4096];
            struct msghdr mess;
            struct sockaddr sa;
            struct iovec vec;
            memset(&mess, 0, sizeof(mess));
            mess.msg_name = &sa;
            mess.msg_namelen = sizeof(sa);
            vec.iov_base = buf;
            vec.iov_len = sizeof(buf);
            mess.msg_iov = &vec;
            mess.msg_iovlen = 1;
            ssize_t rret;
            while ((rret = recvmsg(fd, &mess, 0)) <= 0)
                ;
            if (verbosity >= 2)
                hexdump("recvmsg", buf, rret);
            quicly_decoded_packet_t packet;
            if (quicly_decode_packet(&packet, buf, rret) == 0) {
                if (packet.has_connection_id) {
                    quicly_conn_t *conn = NULL;
                    size_t i;
                    for (i = 0; i != num_conns; ++i) {
                        if (quicly_get_connection_id(conns[i]) == packet.connection_id) {
                            conn = conns[i];
                            break;
                        }
                    }
                    if (conn != NULL) {
                        /* existing connection */
                        quicly_receive(conn, &packet);
                        if (quicly_get_state(conn) == QUICLY_STATE_1RTT_ENCRYPTED && quicly_get_next_stream_id(conn) == 1) {
                            quicly_stream_t *stream;
                            ret = quicly_open_stream(conn, &stream);
                            assert(ret == 0);
                            stream->on_update = on_resp_receive;
                            send_data(stream, "GET / HTTP/1.0\r\n\r\n");
                        }
                    } else {
                        /* new connection */
                        if (quicly_accept(&conn, &ctx, &sa, mess.msg_namelen, NULL, &packet) == 0) {
                            assert(conn != NULL);
                            conns = realloc(conns, sizeof(*conns) * (num_conns + 1));
                            assert(conns != NULL);
                            conns[num_conns++] = conn;
                        } else {
                            assert(conn == NULL);
                        }
                    }
                    if (conn != NULL && send_pending(fd, conn) != 0) {
                        for (i = 0; i != num_conns; ++i) {
                            if (conns[i] == conn) {
                                memcpy(conns + i, conns + i + 1, (num_conns - i - 1) * sizeof(*conns));
                                --num_conns;
                                break;
                            }
                        }
                        quicly_free(conn);
                    }
                } else {
                    fprintf(stderr, "ignoring packet without connection-id\n");
                }
            }
        }
    }
}

static void usage(const char *cmd)
{
    printf("Usage: %s [options] host port\n"
           "\n"
           "Options:\n"
           "  -c certificate-file\n"
           "  -k key-file          specifies the credentials to be used for running the\n"
           "                       server. If omitted, the command runs as a client.\n"
           "  -l log-file          file to log traffic secrets\n"
           "  -r [initial-rto]     initial RTO (in milliseconds)\n"
           "  -s [secret]          use stateless reset\n"
           "  -V                   verify peer using the default certificates\n"
           "  -v                   verbose mode (-vv emits packet dumps as well)\n"
           "  -h                   print this help\n"
           "\n",
           cmd);
}

int main(int argc, char **argv)
{
    const char *host, *port;
    struct sockaddr_storage sa;
    socklen_t salen;
    int ch;

    while ((ch = getopt(argc, argv, "c:k:l:r:s:Vvh")) != -1) {
        switch (ch) {
        case 'c':
            load_certificate_chain(&tlsctx, optarg);
            break;
        case 'k':
            load_private_key(&tlsctx, optarg);
            break;
        case 'l':
            setup_log_secret(&tlsctx, optarg);
            break;
        case 'r':
            if (sscanf(optarg, "%" PRIu32, &ctx.initial_rto) != 1) {
                fprintf(stderr, "invalid argument passed to `-r`\n");
                exit(1);
            }
            break;
        case 's':
            ctx.stateless_retry.enforce_use = 1;
            ctx.stateless_retry.key = optarg;
            if (strlen(ctx.stateless_retry.key) < tlsctx.cipher_suites[0]->hash->digest_size) {
                fprintf(stderr, "secret for stateless retry is too short (should be at least %zu bytes long)\n",
                        tlsctx.cipher_suites[0]->hash->digest_size);
                exit(1);
            }
            break;
        case 'V':
            setup_verify_certificate(&tlsctx);
            break;
        case 'v':
            ++verbosity;
            break;
        default:
            usage(argv[0]);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    if (verbosity != 0)
        ctx.debug_log = quicly_default_debug_log;

    if (tlsctx.certificates.count != 0 || tlsctx.sign_certificate != NULL) {
        /* server */
        if (tlsctx.certificates.count == 0 || tlsctx.sign_certificate == NULL) {
            fprintf(stderr, "-ck and -k options must be used together\n");
            exit(1);
        }
    } else {
        /* client */
    }
    if (argc != 2) {
        fprintf(stderr, "missing host and port\n");
        exit(1);
    }
    host = (--argc, *argv++);
    port = (--argc, *argv++);

    if (resolve_address((void *)&sa, &salen, host, port, SOCK_DGRAM, IPPROTO_UDP) != 0)
        exit(1);

    return tlsctx.certificates.count != 0 ? run_server((void *)&sa, salen) : run_client((void *)&sa, salen, host);
}
