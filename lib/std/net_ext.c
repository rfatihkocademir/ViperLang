#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <errno.h>

// Inline helpers (net.so is standalone, can't link to viper binary)
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static long do_sendfile(int out_fd, int in_fd, long offset, long count) {
    off_t off = (off_t)offset;
    ssize_t sent = sendfile(out_fd, in_fd, &off, (size_t)count);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return (long)sent;
}

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

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        return -1;
    }
    
    // Make the server socket non-blocking for async accept
    set_nonblocking(server_fd);
    printf("[AsyncIO] Server socket %d set to non-blocking mode\n", server_fd);

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

int net_http_respond(int client_fd, const char* html) {
    // Build a proper HTTP/1.1 200 response with real CRLF line endings.
    // ViperLang strings don't support escape sequences, so \r\n must be
    // constructed on the C side.
    int content_length = (int)strlen(html);
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_length);
    send(client_fd, header, header_len, 0);
    send(client_fd, html, content_length, 0);
    return 0;
}

void net_close(int fd) {
    close(fd);
}

// ---- Async / Non-blocking Extensions ----------------------------------------

int net_accept_async(int server_fd) {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int client = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
    if (client < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2; // Signal: no pending connections, yield
        }
        return -1; // Real error
    }
    // Make the accepted client socket non-blocking too
    set_nonblocking(client);
    return client;
}

int net_recv_async(int client_fd, char* buffer, int max_len) {
    int received = read(client_fd, buffer, max_len);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2; // Signal: nothing to read yet, yield
        }
        return -1; // Real error
    }
    return received; // 0 = connection closed, >0 = data
}

int net_send_async(int client_fd, const char* message, int length) {
    int sent = send(client_fd, message, length, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2; // Signal: buffer full, yield
        }
        return -1;
    }
    return sent;
}

// Zero-Copy: Serve a static file directly via sendfile()
int net_http_respond_file(int client_fd, const char* filepath) {
    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        // File not found - send 404
        const char* not_found = 
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 9\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Not Found";
        send(client_fd, not_found, strlen(not_found), 0);
        return -1;
    }
    
    struct stat st;
    fstat(file_fd, &st);
    long file_size = st.st_size;
    
    // Determine content type from extension
    const char* content_type = "application/octet-stream";
    const char* ext = strrchr(filepath, '.');
    if (ext) {
        if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) content_type = "text/html; charset=utf-8";
        else if (strcmp(ext, ".css") == 0) content_type = "text/css";
        else if (strcmp(ext, ".js") == 0)  content_type = "application/javascript";
        else if (strcmp(ext, ".json") == 0) content_type = "application/json";
        else if (strcmp(ext, ".png") == 0) content_type = "image/png";
        else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) content_type = "image/jpeg";
        else if (strcmp(ext, ".svg") == 0) content_type = "image/svg+xml";
    }
    
    // Send HTTP headers
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, file_size);
    send(client_fd, header, header_len, 0);
    
    // Zero-Copy: kernel sends file data directly to socket
    long total_sent = 0;
    while (total_sent < file_size) {
        long sent = do_sendfile(client_fd, file_fd, total_sent, file_size - total_sent);
        if (sent <= 0) break;
        total_sent += sent;
    }
    
    close(file_fd);
    printf("[AsyncIO] Zero-Copy: Served '%s' (%ld bytes via sendfile)\n", filepath, total_sent);
    return 0;
}
