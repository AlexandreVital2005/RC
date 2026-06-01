/*
 * download.c - Simple FTP download client for RCOMP / Computer Networks Lab 2
 *
 * Usage:
 *   ./download ftp://[user:password@]host/path/to/file
 *
 * Examples:
 *   ./download ftp://ftp.netlab.fe.up.pt/pub/README
 *   ./download ftp://anonymous:anonymous@ftp.netlab.fe.up.pt/pub/README
 *
 * This program:
 *   - parses an FTP URL
 *   - resolves the hostname with gethostbyname()
 *   - opens the FTP control connection on TCP port 21
 *   - logs in
 *   - enters passive mode (PASV)
 *   - opens the FTP data connection
 *   - downloads one file to the current working directory
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define FTP_PORT 21
#define BUFFER_SIZE 4096
#define RESPONSE_SIZE 8192

struct ftp_url {
    char user[128];
    char password[128];
    char host[256];
    char path[1024];
    char filename[256];
};

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void get_filename_from_path(const char *path, char *filename, size_t size) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;

    if (name[0] == '\0') {
        fprintf(stderr, "Invalid URL: path does not contain a filename\n");
        exit(EXIT_FAILURE);
    }

    snprintf(filename, size, "%s", name);
}

static void parse_url(const char *url, struct ftp_url *out) {
    const char *prefix = "ftp://";
    const char *p;
    const char *slash;
    const char *at;
    const char *colon;
    size_t len;

    memset(out, 0, sizeof(*out));
    snprintf(out->user, sizeof(out->user), "anonymous");
    snprintf(out->password, sizeof(out->password), "anonymous@");

    if (!starts_with(url, prefix)) {
        fprintf(stderr, "Invalid URL: it must start with ftp://\n");
        exit(EXIT_FAILURE);
    }

    p = url + strlen(prefix);
    slash = strchr(p, '/');
    if (slash == NULL || slash[1] == '\0') {
        fprintf(stderr, "Invalid URL: expected ftp://[user:password@]host/path/file\n");
        exit(EXIT_FAILURE);
    }

    /* Optional credentials appear before the first slash and contain '@'. */
    at = memchr(p, '@', (size_t)(slash - p));
    if (at != NULL) {
        colon = memchr(p, ':', (size_t)(at - p));
        if (colon == NULL) {
            fprintf(stderr, "Invalid URL: credentials must be user:password@\n");
            exit(EXIT_FAILURE);
        }

        len = (size_t)(colon - p);
        if (len == 0 || len >= sizeof(out->user)) {
            fprintf(stderr, "Invalid URL: username is empty or too long\n");
            exit(EXIT_FAILURE);
        }
        memcpy(out->user, p, len);
        out->user[len] = '\0';

        len = (size_t)(at - colon - 1);
        if (len == 0 || len >= sizeof(out->password)) {
            fprintf(stderr, "Invalid URL: password is empty or too long\n");
            exit(EXIT_FAILURE);
        }
        memcpy(out->password, colon + 1, len);
        out->password[len] = '\0';

        p = at + 1;
    }

    len = (size_t)(slash - p);
    if (len == 0 || len >= sizeof(out->host)) {
        fprintf(stderr, "Invalid URL: host is empty or too long\n");
        exit(EXIT_FAILURE);
    }
    memcpy(out->host, p, len);
    out->host[len] = '\0';

    /* Store the FTP path without the URL separator slash. */
    snprintf(out->path, sizeof(out->path), "%s", slash + 1);
    get_filename_from_path(out->path, out->filename, sizeof(out->filename));
}

static int connect_tcp_ip(const char *ip, int port) {
    int sockfd;
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address: %s\n", ip);
        exit(EXIT_FAILURE);
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) die("socket");

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        die("connect");
    }

    return sockfd;
}

static int connect_tcp_host(const char *host, int port, char *resolved_ip, size_t ip_size) {
    struct hostent *h;
    struct in_addr *addr;

    h = gethostbyname(host);
    if (h == NULL) {
        fprintf(stderr, "gethostbyname failed for host: %s\n", host);
        exit(EXIT_FAILURE);
    }

    addr = (struct in_addr *)h->h_addr_list[0];
    snprintf(resolved_ip, ip_size, "%s", inet_ntoa(*addr));

    return connect_tcp_ip(resolved_ip, port);
}

static int is_response_end_line(const char *line, int *code_out) {
    if (isdigit((unsigned char)line[0]) &&
        isdigit((unsigned char)line[1]) &&
        isdigit((unsigned char)line[2]) &&
        line[3] == ' ') {
        *code_out = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        return 1;
    }
    return 0;
}

static int read_response(int sockfd, char *response, size_t response_size) {
    size_t total = 0;
    char line[1024];
    size_t line_len = 0;
    int code = -1;

    if (response_size == 0) return -1;
    response[0] = '\0';

    while (1) {
        char c;
        ssize_t n = read(sockfd, &c, 1);
        if (n < 0) die("read response");
        if (n == 0) {
            fprintf(stderr, "Connection closed while reading FTP response\n");
            exit(EXIT_FAILURE);
        }

        if (total + 1 < response_size) {
            response[total++] = c;
            response[total] = '\0';
        }

        if (line_len + 1 < sizeof(line)) {
            line[line_len++] = c;
            line[line_len] = '\0';
        }

        if (c == '\n') {
            int current_code;
            printf("<-- %s", line);

            if (line_len >= 5 && is_response_end_line(line, &current_code)) {
                code = current_code;
                break;
            }

            line_len = 0;
            line[0] = '\0';
        }
    }

    return code;
}

static int send_command(int sockfd, const char *fmt, ...) {
    char command[2048];
    char full_command[2050];
    va_list args;
    size_t len;
    ssize_t written;

    va_start(args, fmt);
    vsnprintf(command, sizeof(command), fmt, args);
    va_end(args);

    snprintf(full_command, sizeof(full_command), "%s\r\n", command);
    len = strlen(full_command);

    printf("--> %s\n", command);
    written = write(sockfd, full_command, len);
    if (written < 0) die("write command");
    if ((size_t)written != len) {
        fprintf(stderr, "Could not write full FTP command\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}

static void expect_code(int code, int expected, const char *phase) {
    if (code != expected) {
        fprintf(stderr, "FTP error in phase '%s': expected %d, got %d\n", phase, expected, code);
        exit(EXIT_FAILURE);
    }
}

static void parse_pasv_response(const char *response, char *ip, size_t ip_size, int *port) {
    const char *p = strchr(response, '(');
    int h1, h2, h3, h4, p1, p2;

    if (p == NULL || sscanf(p, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        fprintf(stderr, "Could not parse PASV response:\n%s\n", response);
        exit(EXIT_FAILURE);
    }

    if (h1 < 0 || h1 > 255 || h2 < 0 || h2 > 255 || h3 < 0 || h3 > 255 || h4 < 0 || h4 > 255 ||
        p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) {
        fprintf(stderr, "Invalid numbers in PASV response\n");
        exit(EXIT_FAILURE);
    }

    snprintf(ip, ip_size, "%d.%d.%d.%d", h1, h2, h3, h4);
    *port = p1 * 256 + p2;
}

static void download_data(int data_sock, const char *filename) {
    FILE *file;
    char buffer[BUFFER_SIZE];
    ssize_t n;
    long total = 0;

    file = fopen(filename, "wb");
    if (file == NULL) die("fopen");

    while ((n = read(data_sock, buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) {
            fclose(file);
            fprintf(stderr, "Error writing to local file\n");
            exit(EXIT_FAILURE);
        }
        total += n;
    }

    if (n < 0) {
        fclose(file);
        die("read data");
    }

    if (fclose(file) != 0) die("fclose");
    printf("Downloaded %ld bytes to '%s'\n", total, filename);
}

int main(int argc, char **argv) {
    struct ftp_url url;
    char server_ip[64];
    char pasv_ip[64];
    char response[RESPONSE_SIZE];
    int control_sock;
    int data_sock;
    int pasv_port;
    int code;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s ftp://[user:password@]host/path/file\n", argv[0]);
        return EXIT_FAILURE;
    }

    parse_url(argv[1], &url);

    printf("User     : %s\n", url.user);
    printf("Host     : %s\n", url.host);
    printf("Path     : %s\n", url.path);
    printf("Filename : %s\n", url.filename);

    control_sock = connect_tcp_host(url.host, FTP_PORT, server_ip, sizeof(server_ip));
    printf("Connected to %s (%s) on port %d\n", url.host, server_ip, FTP_PORT);

    code = read_response(control_sock, response, sizeof(response));
    expect_code(code, 220, "server greeting");

    send_command(control_sock, "USER %s", url.user);
    code = read_response(control_sock, response, sizeof(response));
    if (code == 331) {
        send_command(control_sock, "PASS %s", url.password);
        code = read_response(control_sock, response, sizeof(response));
        expect_code(code, 230, "login password");
    } else if (code != 230) {
        fprintf(stderr, "FTP error in phase 'login user': expected 331 or 230, got %d\n", code);
        exit(EXIT_FAILURE);
    }

    send_command(control_sock, "TYPE I");
    code = read_response(control_sock, response, sizeof(response));
    expect_code(code, 200, "binary mode");

    send_command(control_sock, "PASV");
    code = read_response(control_sock, response, sizeof(response));
    expect_code(code, 227, "passive mode");
    parse_pasv_response(response, pasv_ip, sizeof(pasv_ip), &pasv_port);
    printf("Passive data endpoint: %s:%d\n", pasv_ip, pasv_port);

    data_sock = connect_tcp_ip(pasv_ip, pasv_port);

    send_command(control_sock, "RETR %s", url.path);
    code = read_response(control_sock, response, sizeof(response));
    if (code != 150 && code != 125) {
        fprintf(stderr, "FTP error in phase 'retrieve': expected 150 or 125, got %d\n", code);
        close(data_sock);
        exit(EXIT_FAILURE);
    }

    download_data(data_sock, url.filename);
    if (close(data_sock) < 0) die("close data socket");

    code = read_response(control_sock, response, sizeof(response));
    expect_code(code, 226, "transfer complete");

    send_command(control_sock, "QUIT");
    code = read_response(control_sock, response, sizeof(response));
    expect_code(code, 221, "quit");

    if (close(control_sock) < 0) die("close control socket");

    return EXIT_SUCCESS;
}
