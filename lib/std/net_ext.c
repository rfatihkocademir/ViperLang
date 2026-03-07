#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// High-level C extension for ViperLang Network operations

int net_tcp_server(int port) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        return -1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return -1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return -1;
    }

    return server_fd;
}

int net_accept(int server_fd) {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
    return socket;
}

int net_send(int client_fd, const char* message) {
    return send(client_fd, message, strlen(message), 0);
}

int net_recv(int client_fd, char* buffer, int max_len) {
    return read(client_fd, buffer, max_len);
}

static char recv_buf[8192];
char* net_recv_string(int client_fd) {
    int received = read(client_fd, recv_buf, sizeof(recv_buf) - 1);
    if (received <= 0) {
        return NULL;
    }
    recv_buf[received] = '\0';
    return recv_buf;
}

void net_close(int fd) {
    close(fd);
}
