/*
 * Minimal HTTP/1.1 backend for the scientific calculator.
 *
 *   GET /api/eval?expr=...&angle=deg|rad|grad&ans=...&mem=...
 *   GET /api/health
 *   GET /<file>            static frontend from the web root (default ./public)
 *
 * Also usable from the terminal:  calc -e "sin(30)" -d   or   calc -i  (REPL)
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "calc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define REQ_MAX       16384
#define EXPR_MAX      4096
#define FILE_MAX      (8 * 1024 * 1024)

static const char *web_root = "public";
static volatile sig_atomic_t running = 1;

static const char *const ANGLE_NAMES[] = {"rad", "deg", "grad"};

/* ------------------------------------------------------------ utilities */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, size_t len, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && i + 2 < len && hexval(src[i + 1]) >= 0 && hexval(src[i + 2]) >= 0) {
            dst[o++] = (char)(hexval(src[i + 1]) * 16 + hexval(src[i + 2]));
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* Find `key` in an application/x-www-form-urlencoded query string and decode its value. */
static int query_param(const char *query, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *end = strchr(p, '&');
        size_t seg = end ? (size_t)(end - p) : strlen(p);
        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            url_decode(p + klen + 1, seg - klen - 1, out, cap);
            return 1;
        }
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static double parse_finite(const char *s) {
    char *end;
    double v = strtod(s, &end);
    return (end != s && isfinite(v)) ? v : 0;
}

/* ----------------------------------------------------------- responses */

static void send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

static const char *reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default:  return "Internal Server Error";
    }
}

static void respond(int fd, int status, const char *ctype, const char *body, size_t len, int head_only) {
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Connection: close\r\n\r\n",
                     status, reason_phrase(status), ctype, len);
    send_all(fd, hdr, (size_t)n);
    if (!head_only && len > 0) send_all(fd, body, len);
}

static void respond_json(int fd, int status, const char *json, int head_only) {
    respond(fd, status, "application/json; charset=utf-8", json, strlen(json), head_only);
}

/* -------------------------------------------------------------- routes */

static void handle_eval(int fd, const char *query, int head_only) {
    char expr[EXPR_MAX], param[64], err[256], shown[64], esc[EXPR_MAX * 2], body[EXPR_MAX * 2 + 256];
    CalcContext ctx = {ANGLE_RAD, 0, 0};
    double v;

    if (!query_param(query, "expr", expr, sizeof expr)) {
        respond_json(fd, 400, "{\"ok\":false,\"error\":\"Missing 'expr' parameter\"}", head_only);
        return;
    }
    if (query_param(query, "angle", param, sizeof param)) {
        if (strcasecmp(param, "deg") == 0) ctx.angle = ANGLE_DEG;
        else if (strcasecmp(param, "grad") == 0) ctx.angle = ANGLE_GRAD;
    }
    if (query_param(query, "ans", param, sizeof param)) ctx.ans = parse_finite(param);
    if (query_param(query, "mem", param, sizeof param)) ctx.mem = parse_finite(param);

    if (calc_eval(expr, &ctx, &v, err, sizeof err) == 0) {
        calc_format(v, shown, sizeof shown);
        snprintf(body, sizeof body, "{\"ok\":true,\"result\":\"%s\",\"raw\":\"%.17g\"}", shown, v == 0 ? 0.0 : v);
        printf("eval [%s] %s = %s\n", ANGLE_NAMES[ctx.angle], expr, shown);
    } else {
        json_escape(err, esc, sizeof esc);
        snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", esc);
        printf("eval [%s] %s -> error: %s\n", ANGLE_NAMES[ctx.angle], expr, err);
    }
    fflush(stdout);
    respond_json(fd, 200, body, head_only);
}

static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js")) return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

static void serve_static(int fd, const char *path, int head_only) {
    char full[1024];
    struct stat st;
    FILE *f;
    char *buf;
    size_t n;

    if (strcmp(path, "/") == 0) path = "/index.html";
    for (const char *c = path; *c; c++) {
        if (!isalnum((unsigned char)*c) && !strchr("/._-", *c)) {
            respond(fd, 404, "text/plain; charset=utf-8", "Not found", 9, head_only);
            return;
        }
    }
    if (strstr(path, "..")) {
        respond(fd, 403, "text/plain; charset=utf-8", "Forbidden", 9, head_only);
        return;
    }

    snprintf(full, sizeof full, "%s%s", web_root, path);
    f = fopen(full, "rb");
    if (!f || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > FILE_MAX) {
        if (f) fclose(f);
        respond(fd, 404, "text/plain; charset=utf-8", "Not found", 9, head_only);
        return;
    }
    buf = malloc(st.st_size > 0 ? (size_t)st.st_size : 1);
    if (!buf) {
        fclose(f);
        respond(fd, 500, "text/plain; charset=utf-8", "Out of memory", 13, head_only);
        return;
    }
    n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    respond(fd, 200, mime_type(full), buf, n, head_only);
    free(buf);
}

static void handle_client(int fd) {
    struct timeval tv = {5, 0};
    char req[REQ_MAX + 1], method[8], target[8192];
    const char *query = "";
    char *q;
    size_t len = 0;
    int head_only;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (len < REQ_MAX) {
        ssize_t n = recv(fd, req + len, REQ_MAX - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
        req[len] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (len == 0) return;
    req[len] = '\0';

    if (sscanf(req, "%7s %8191s", method, target) != 2) {
        respond(fd, 400, "text/plain; charset=utf-8", "Bad request", 11, 0);
        return;
    }
    if (strcmp(method, "OPTIONS") == 0) {
        respond(fd, 204, "text/plain", "", 0, 1);
        return;
    }
    head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        respond_json(fd, 405, "{\"ok\":false,\"error\":\"Method not allowed\"}", 0);
        return;
    }

    if ((q = strchr(target, '?')) != NULL) {
        *q = '\0';
        query = q + 1;
    }

    if (strcmp(target, "/api/eval") == 0)
        handle_eval(fd, query, head_only);
    else if (strcmp(target, "/api/health") == 0)
        respond_json(fd, 200, "{\"ok\":true,\"service\":\"c-scientific-calculator\",\"version\":\"1.0\"}", head_only);
    else if (strncmp(target, "/api/", 5) == 0)
        respond_json(fd, 404, "{\"ok\":false,\"error\":\"Unknown endpoint\"}", head_only);
    else
        serve_static(fd, target, head_only);
}

/* ---------------------------------------------------------------- modes */

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

static int run_server(int port) {
    struct sigaction sa;
    struct sockaddr_in addr;
    char index_path[1024];
    int srv, one = 1;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART, so accept() returns on Ctrl+C */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(srv);
        return 1;
    }
    if (listen(srv, 64) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    snprintf(index_path, sizeof index_path, "%s/index.html", web_root);
    if (access(index_path, R_OK) != 0)
        fprintf(stderr, "warning: %s not found; run from the project directory or pass -r <web root>\n", index_path);

    printf("Scientific calculator running at http://localhost:%d\n", port);
    printf("Serving frontend from %s/  (Ctrl+C to stop)\n", web_root);
    fflush(stdout);

    while (running) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR) perror("accept");
            continue;
        }
        handle_client(fd);
        close(fd);
    }

    close(srv);
    printf("\nServer stopped.\n");
    return 0;
}

static int run_cli(const char *expr, AngleMode angle) {
    CalcContext ctx = {angle, 0, 0};
    char err[256], out[64];
    double v;

    if (calc_eval(expr, &ctx, &v, err, sizeof err) != 0) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    calc_format(v, out, sizeof out);
    puts(out);
    return 0;
}

static int run_repl(AngleMode angle) {
    CalcContext ctx = {angle, 0, 0};
    char line[EXPR_MAX], err[256], out[64];
    double v;

    printf("Scientific calculator REPL. Commands: deg, rad, grad, quit\n");
    for (;;) {
        printf("[%s] > ", ANGLE_NAMES[ctx.angle]);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';

        if (!strcmp(line, "quit") || !strcmp(line, "exit")) break;
        if (!strcmp(line, "deg")) { ctx.angle = ANGLE_DEG; continue; }
        if (!strcmp(line, "rad")) { ctx.angle = ANGLE_RAD; continue; }
        if (!strcmp(line, "grad")) { ctx.angle = ANGLE_GRAD; continue; }
        if (line[0] == '\0') continue;

        if (calc_eval(line, &ctx, &v, err, sizeof err) == 0) {
            calc_format(v, out, sizeof out);
            printf("= %s\n", out);
            ctx.ans = v;
        } else {
            printf("Error: %s\n", err);
        }
    }
    putchar('\n');
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  (no options)     start the web server on port 8080\n"
            "  -p <port>        server port\n"
            "  -r <dir>         directory holding the frontend (default: public)\n"
            "  -e <expr>        evaluate one expression and print the result\n"
            "  -i               interactive REPL\n"
            "  -d | -g          use degrees | gradians for -e / -i (default radians)\n"
            "  -h               show this help\n",
            prog);
}

int main(int argc, char **argv) {
    AngleMode angle = ANGLE_RAD;
    const char *expr = NULL;
    int port = 8080, repl = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) web_root = argv[++i];
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) expr = argv[++i];
        else if (!strcmp(argv[i], "-i")) repl = 1;
        else if (!strcmp(argv[i], "-d")) angle = ANGLE_DEG;
        else if (!strcmp(argv[i], "-g")) angle = ANGLE_GRAD;
        else {
            usage(argv[0]);
            return strcmp(argv[i], "-h") == 0 ? 0 : 2;
        }
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    if (expr) return run_cli(expr, angle);
    if (repl) return run_repl(angle);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return 2;
    }
    return run_server(port);
}
