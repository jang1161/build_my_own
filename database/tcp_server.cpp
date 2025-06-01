#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <unistd.h>
#include <cassert>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <poll.h>
#include <fcntl.h>

#include <vector>
using namespace std;

const size_t K_MAX_MSG = 32 << 20;

struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    vector<uint8_t> incoming; // data from the socket
    vector<uint8_t> outgoing; // generated responses
};

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void buf_append(vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

// Request if there is enough data
static bool try_one_request(Conn *conn) {
    // Try to parse the accumulated buffer
    if(conn->incoming.size() < 4) {
        return false; // want read more
    }

    // Protocol: message header
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if(len > K_MAX_MSG) {
        msg("too long");
        conn->want_close = true;
        return false; // want close
    }

    // Protocol: message body
    if(4 + len > conn->incoming.size()) {
        return false; // want read more
    }
    const uint8_t *request = &conn->incoming[4];

    // Got one request, do something
    printf("client says: len: %d, data: %.*s\n",
    len, len < 100 ? len : 100, request);

    // Generate the response (echo)
    buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    buf_append(conn->outgoing, request, len);

    // Remove the message from incoming
    buf_consume(conn->incoming, 4 + len);
    return true;
}

static int32_t read_full(int fd, char *buf, size_t n) {
    while(n > 0) {
        ssize_t rv = read(fd, buf, n);
        if(rv <= 0) { // error
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while(n > 0) {
        ssize_t rv = write(fd, buf, n);
        if(rv <= 0) { // error
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static void fd_set_nb(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static Conn *handle_accept(int fd) { // fd: server's listen socket
    // Accept
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if(connfd < 0)
        return NULL;

    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port)
    );

    // Set new connection to non-bloking mode
    fd_set_nb(connfd);

    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true; // read the 1st request
    return conn;
}

static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
    if(rv < 0 && errno == EAGAIN) {
        return; // client not ready (send buffer is full)
    }
    if(rv < 0) {
        conn->want_close = true;
        return;
    }

    // Remove written data from outgoing
    buf_consume(conn->outgoing, (size_t)rv);

    // Update the readiness intention
    if(conn->outgoing.size() == 0) { // all data written
        conn->want_read = true;
        conn->want_write = false;
    } // else: want write
}

static void handle_read(Conn *conn) {
    // Do a non-blocking read
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    
    if(rv < 0 && errno == EAGAIN) {
        return; // not ready (receive buffer is empty)
    }
    if(rv <= 0) { // IO error (rv < 0) or EOF (rv == 0)
        conn->want_close = true;
        return;
    }

    // Add new data to the incoming buffer
    buf_append(conn->incoming, buf, (size_t)rv);

    // Parse requests and generate responses
    while(try_one_request(conn)) {}

    // Update the readiness intention
    if(conn->outgoing.size() > 0 ) { // has a response
        conn->want_read = false;
        conn->want_write = true;
        return handle_write(conn); // optimization
    } // else: wand read
}

int main() {
    // Obtain a socket handle
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); // TIME_WAIT 상태의 포트도 즉시 재사용 가능하게 설정

    // Set socket options
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234); // port - Host(little-endian) to Network(big-endian) Long
    addr.sin_addr.s_addr = ntohl(0);  // wildcard address 0.0.0.0

    // Bind to an address
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // Listen
    rv = listen(fd, SOMAXCONN); // 2nd arg: size of the queue, SOMAXCONN = 4096
    if (rv) {
        die("listen()");
    }

    vector<Conn *> fd2conn; // Map of all client connections, index = fd
    vector<struct pollfd> poll_args; // Event loop

    while (true) {
        // Construct the fd list for poll()
        poll_args.clear(); // reset
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd); // Put the listening socket in the first

        for(Conn *conn : fd2conn) { // Rest are connections sockets
            if(!conn) {
                continue;
            }
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if(conn->want_read) 
                pfd.events |= POLLIN;
            if(conn->want_write) 
                pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        // Call poll()
        rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if(rv < 0 && errno == EINTR) {
            continue; // not an error (interrupt)
        }
        if(rv < 0) {
            die("poll()");
        }

        // Accept new connections
        if(poll_args[0].revents) {
            if(Conn *conn = handle_accept(fd)) {
                if(fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                fd2conn[conn->fd] = conn;
            }
        }

        // Invoke application callbacks
        for(size_t i = 1; i < poll_args.size(); i++) { // skip the 1st
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];
            if(ready & POLLIN) {
                handle_read(conn);
            }
            if(ready & POLLOUT) {
                handle_write(conn);
            }

            // Terminate connections
            if((ready & POLLERR) || conn->want_close) {
                close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }
        }
    }

    return 0;
}