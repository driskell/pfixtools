/******************************************************************************/
/*          pfixtools: a collection of postfix related tools                  */
/*          ~~~~~~~~~                                                         */
/*  ________________________________________________________________________  */
/*                                                                            */
/*  Redistribution and use in source and binary forms, with or without        */
/*  modification, are permitted provided that the following conditions        */
/*  are met:                                                                  */
/*                                                                            */
/*  1. Redistributions of source code must retain the above copyright         */
/*     notice, this list of conditions and the following disclaimer.          */
/*  2. Redistributions in binary form must reproduce the above copyright      */
/*     notice, this list of conditions and the following disclaimer in the    */
/*     documentation and/or other materials provided with the distribution.   */
/*  3. The names of its contributors may not be used to endorse or promote    */
/*     products derived from this software without specific prior written     */
/*     permission.                                                            */
/*                                                                            */
/*  THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND   */
/*  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE     */
/*  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR        */
/*  PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS    */
/*  BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR    */
/*  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF      */
/*  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS  */
/*  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN   */
/*  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)   */
/*  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF    */
/*  THE POSSIBILITY OF SUCH DAMAGE.                                           */
/******************************************************************************/

/*
 * Copyright © 2008 Florent Bruneau
 */

#include "server.h"
#include "epoll.h"
#include "common.h"

static PA(server_t) listeners   = ARRAY_INIT;
static PA(server_t) server_pool = ARRAY_INIT;

static server_t* server_new(void)
{
    server_t* server = p_new(server_t, 1);
    server->fd  = -1;
    server->fd2 = -1;
    return server;
}

static void server_wipe(server_t *server)
{
    server->listener = server->event = false;
    if (server->fd > 0) {
        epoll_modify(server->fd, 0, NULL);
        close(server->fd);
        server->fd = -1;
    }
    if (server->fd2 > 0) {
        close(server->fd2);
        server->fd2 = -1;
    }
    if (server->data && server->clear_data) {
        server->clear_data(&server->data);
    }
}

static void server_delete(server_t **server)
{
    if (*server) {
        buffer_wipe(&(*server)->ibuf);
        buffer_wipe(&(*server)->obuf);
        server_wipe(*server);
        p_delete(server);
    }
}

static server_t* server_acquire(void)
{
    if (server_pool.len != 0) {
        return array_elt(server_pool, --server_pool.len);
    } else {
        return server_new();
    }
}

static void server_release(server_t *server)
{
    server_wipe(server);
    array_add(server_pool, server);
}

static void server_shutdown(void)
{
    array_deep_wipe(listeners, server_delete);
    array_deep_wipe(server_pool, server_delete);
}

module_exit(server_shutdown);

int start_server(int port, start_listener_t starter, delete_client_t deleter)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr   = { htonl(INADDR_LOOPBACK) },
    };
    server_t *tmp;
    void* data = NULL;
    int sock;

    addr.sin_port = htons(port);
    sock = tcp_listen_nonblock((const struct sockaddr *)&addr, sizeof(addr));
    if (sock < 0) {
        return -1;
    }

    if (starter) {
      data = starter();
      if (data == NULL) {
        close(sock);
        return -1;
      }
    }

    tmp             = server_acquire();
    tmp->fd         = sock;
    tmp->listener   = true;
    tmp->data       = data;
    tmp->clear_data = deleter;
    epoll_register(sock, EPOLLIN, tmp);
    array_add(listeners, tmp);
    return 0;
}

static int start_client(server_t *server, start_client_t starter,
                        delete_client_t deleter)
{
    server_t *tmp;
    void* data = NULL;
    int sock;

    sock = accept_nonblock(server->fd);
    if (sock < 0) {
        UNIXERR("accept");
        return -1;
    }

    if (starter) {
        data = starter(server);
        if (data == NULL) {
            close(sock);
            return -1;
        }
    }

    tmp             = server_acquire();
    tmp->fd         = sock;
    tmp->data       = data;
    tmp->clear_data = deleter;
    epoll_register(sock, EPOLLIN, tmp);
    return 0;
}

event_t event_register(int fd, void *data)
{
    int fds[2];
    if (fd == -1) {
        if (pipe(fds) != 0) {
            UNIXERR("pipe");
            return NULL;
        }
        if (setnonblock(fds[0]) != 0) {
            close(fds[0]);
            close(fds[1]);
            return NULL;
        }
    }

    server_t *tmp = server_acquire();
    tmp->event = true;
    tmp->fd    = fd == -1 ? fds[0] : fd;
    tmp->fd2   = fd == -1 ? fds[1] : -1;
    tmp->data  = data;
    epoll_register(fds[0], EPOLLIN, tmp);
    return tmp;
}

bool event_fire(event_t event)
{
    static const char *data = "";
    if (event->fd2 == -1) {
        return false;
    }
    return write(event->fd2, data, 1) == 0;
}

static bool event_cancel(event_t event)
{
    char buff[32];
    while (true) {
        ssize_t res = read(event->fd, buff, 32);
        if (res == -1 && errno != EAGAIN && errno != EINTR) {
            UNIXERR("read");
            return false;
        } else if (res == -1 && errno == EINTR) {
            continue;
        } else if (res != 32) {
            return true;
        }
    }
}

int server_loop(start_client_t starter, delete_client_t deleter,
                run_client_t runner, event_handler_t handler,
                refresh_t refresh, void* config)
{
    info("entering processing loop");
    while (!sigint) {
        struct epoll_event evts[1024];
        int n;

        if (sighup && refresh) {
            sighup = false;
            info("refreshing...");
            if (!refresh(config)) {
                crit("error while refreshing configuration");
                return EXIT_FAILURE;
            }
            info("refresh done, processing loop restarts");
        }

        n = epoll_select(evts, countof(evts), -1);
        if (n < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                UNIXERR("epoll_wait");
                return EXIT_FAILURE;
            }
            continue;
        }

        while (--n >= 0) {
            server_t *d = evts[n].data.ptr;

            if (d->listener) {
                (void)start_client(d, starter, deleter);
                continue;
            } else if (d->event) {
                if (!event_cancel(d)) {
                    server_release(d);
                    continue;
                }
                if (handler) {
                    if (!handler(d, config)) {
                        server_release(d);
                    }
                }
                continue;
            }

            if (evts[n].events & EPOLLIN) {
                if (runner(d, config) < 0) {
                    server_release(d);
                    continue;
                }
            }

            if ((evts[n].events & EPOLLOUT) && d->obuf.len) {
                if (buffer_write(&d->obuf, d->fd) < 0) {
                    server_release(d);
                    continue;
                }
                if (!d->obuf.len) {
                    epoll_modify(d->fd, EPOLLIN, d);
                }
            }
        }
    }
    info("exit requested");
    return EXIT_SUCCESS;
}
