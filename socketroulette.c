/*
 * socketroulette - a server that pipes pairs of peers
 * © 2016 Charles Lehner
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <err.h>
#include <errno.h>

#define MAX_CLIENTS 1024

int clients[MAX_CLIENTS]; // map fds to fds

int epoll;
int listener;
int client_waiting = -1;

int verbosity = 0;

int server_listen(int port) {
    int fd;
    struct sockaddr_in6 server_addr;
    struct sockaddr *addr = (struct sockaddr *)&server_addr;
    int reuse = -1;

    // Create the proxy server socket
    if ((fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0) {
        warn("failed to create socket");
        return -1;
    }

    // Build the address to listen on
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(port);

    // Prevent socket reuse errors
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse)) {
        warn("setsockopt failed");
        return -1;
    }

    // Bind to the given port/address
    if (bind(fd, addr, sizeof(server_addr))) {
        warn("bind");
        return -1;
    }

    // Listen for clients
    if (listen(fd, 7)) {
        warn("listen");
        return -1;
    }

    return fd;
}

int add_fd_to_epoll(int fd) {
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLET,
        .data.fd = fd
    };

    if (epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event) < 0) {
        warn("epoll_ctl");
        return -1;
    }

    return 0;
}

int remove_fd_from_epoll(int fd) {
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLET,
        .data.fd = fd
    };

    if (epoll_ctl(epoll, EPOLL_CTL_DEL, fd, &event) < 0) {
        warn("epoll_ctl");
        return -1;
    }

    return 0;
}

int read_client(int fd) {
    ssize_t len = 0;
    char buf[1024];
    int peer = clients[fd];

    len = recv(fd, buf, sizeof buf, 0);
    if (verbosity >= 2)
        printf("read %u. len %zd\n", fd, len);
    if (len < 0) {
        // error
        if (errno != EAGAIN)
            warn("recv");
        return -1;

    } else if (len == 0) {
        if (verbosity)
            printf("client %zu disconnected\n", fd);
        // fd is automatically removed from epoll set when the socket closes
        // Remove fd from epoll
        remove_fd_from_epoll(fd);

        // Close peer connection
        if (peer > -1) {
            if (shutdown(peer, SHUT_RDWR) < 0)
                warn("shutdown");
            clients[fd] = -1;
            clients[peer] = -1;
            remove_fd_from_epoll(peer);
        }
    } else {
        if (peer == -1) {
            warnx("no peer");
            return 0;
        }

        // Transfer data to peer
        size_t sv = 0;
        while (sv < len) {
            ssize_t bytes = send(peer, buf + sv, len - sv, 0);
            if (bytes < 0) {
                warn("error sending data (%zu-%zu)", fd, peer);
                // lets's say the peer has disconnected.
                clients[peer] = -1;
                clients[fd] = -1;
                remove_fd_from_epoll(fd);
                remove_fd_from_epoll(peer);
                if (shutdown(peer, SHUT_WR) < 0)
                    warn("shutdown1");
                if (shutdown(fd, SHUT_RD) < 0)
                    warn("shutdown2");
                break;
            }
            sv += bytes;
        }
    }
    return 0;
}

int accept_connection() {
    int fd;
    struct sockaddr addr;
    socklen_t addrlen = sizeof addr;

    if ((fd = accept4(listener, &addr, &addrlen, SOCK_NONBLOCK)) < 0) {
        warn("accept");
        return -1;
    }

	struct sockaddr_in6 *addr6 = (void *)&addr;
	char addr_str[INET6_ADDRSTRLEN + 1] = "";
    if (!inet_ntop(AF_INET6, &addr6->sin6_addr, addr_str, INET6_ADDRSTRLEN))
        warn("inet_ntop");
    if (verbosity)
        printf("client %zu connected from %s\n", fd, addr_str);

    if (fd > MAX_CLIENTS) {
        warnx("too many clients");
        if (shutdown(fd, SHUT_RDWR) < 0)
            warn("shutdown3");
        return -1;
    }

    if (client_waiting >= 0) {
        // Pair up with waiting client
        clients[client_waiting] = fd;
        clients[fd] = client_waiting;

        // Start moving data
        add_fd_to_epoll(client_waiting);
        add_fd_to_epoll(fd);
        read_client(client_waiting);
        client_waiting = -1;
    } else {
        // We are the waiting client
        client_waiting = fd;
        clients[fd] = -1;
    }

    return 0;
}

int main(int argc, char *argv[argc])
{
    const char *port = NULL;

    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-')
            for (char *arg = &argv[i][1]; *arg == 'v'; arg++)
                verbosity++;
        else if (port)
            argc = 1;
        else
            port = argv[i];

    if (argc <= 1)
        errx(1, "Usage: %s [-v] <port>", argv[0]);

    epoll = epoll_create1(0);
    if (epoll < 0)
        err(1, "failed to create epoll fd");

    listener = server_listen(atoi(port));
    if (listener < 0)
        return 1;
    if (verbosity > 0)
        printf("Listening on [::]:%s\n", port);

    if (add_fd_to_epoll(listener) < 0)
        return 1;

	signal(SIGPIPE, SIG_IGN);

    for (;;) {
        // Wait for events on the fds
        static const int events_per_poll = 8;
        struct epoll_event events[events_per_poll];
        int num_ready = epoll_wait(epoll, events, events_per_poll, -1);
        if (num_ready < 0) {
            if (errno == EINTR)
                continue;
            err(1, "epoll_wait");
        }

        // Handle the events
        for (int i = 0; i < num_ready; i++) {
            if (events[i].data.fd == listener) {
                // Handle new client
                accept_connection();
            } else {
                // Handle data from client
                read_client(events[i].data.fd);
            }
        }
    }

    return 1;
}
