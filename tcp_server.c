#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>

void die(const char *msg) {
    printf("%s\n", msg);
}

static void do_something(int connfd) {
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1); // n: 실제로 읽은 바이트 수, EOF 0, 에러 -1
    if(n<0) {
        die("read() error");
        return;
    }
    printf("client says: %s\n", rbuf);

    char wbuf[] = "world";
    write(connfd, wbuf, strlen(wbuf));
}

int main() {
    // Obtain a socket handle
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); // TIME_WAIT 상태의 포트도 즉시 재사용 가능하게 설정

    // Set socket options
    struct sockaddr_in addr = {};
    /* 
    struct sockaddr_in {
        uint16_t       sin_family; // AF_INET
        uint16_t       sin_port;   // port in big-endian
        struct in_addr sin_addr;   // IPv4
    };
    struct in_addr {
        uint32_t       s_addr;     // IPv4 in big-endian
    };
    */
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234); // port - Host(little-endian) to Network(big-endian) Long
    addr.sin_addr.s_addr = htonl(0); // wildcard IP 0.0.0.0

    // Bind to an address
    int rv = bind(fd, (struct sockaddr *)&addr, sizeof(addr)); // 성공시 0, 실패시 -1
    if(rv) {die("bind()");}

    // Listen
    rv = listen(fd, SOMAXCONN); // 2nd arg: size of the queue, SOMAXCONN = 4096
    if(rv) {die("listen()");}

    // Accept connections
    while(1) {
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);
        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
        if(connfd < 0 ) { // error
            continue;
        }

        do_something(connfd);
        close(connfd);
    }
}