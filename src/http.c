/*
 * Tiny HTTP server for the dashboard (priority 15).
 *
 *   GET  /api/state            one JSON snapshot
 *   GET  /api/stream           Server-Sent Events, 10 Hz
 *   POST /api/cmd?do=<cmd>     door_open | power_off | stress_on | reset | ...
 *   GET  /<file>               static files from --web <dir> (optional)
 *
 * The dashboard can therefore be served by the QNX target itself.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char g_webroot[256];

#define CORS "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n" \
             "Access-Control-Allow-Headers: Content-Type\r\n"

static int send_all(int fd, const char *b, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, b, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

static void reply(int fd, int code, const char *type, const char *body, size_t len)
{
    char h[512];
    const char *txt = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request"
                    : code == 404 ? "Not Found" : "Error";
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        CORS "Cache-Control: no-store\r\nConnection: close\r\n\r\n", code, txt, type, len);
    if (send_all(fd, h, (size_t)n) == 0 && len) send_all(fd, body, len);
}

static const char *mime(const char *p)
{
    const char *e = strrchr(p, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".js"))   return "text/javascript; charset=utf-8";
    if (!strcmp(e, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".svg"))  return "image/svg+xml";
    if (!strcmp(e, ".png"))  return "image/png";
    if (!strcmp(e, ".ico"))  return "image/x-icon";
    if (!strcmp(e, ".woff2"))return "font/woff2";
    return "application/octet-stream";
}

static void serve_file(int fd, const char *path)
{
    char full[800];
    struct stat st;
    if (!g_webroot[0] || strstr(path, "..")) { reply(fd, 404, "text/plain", "not found\n", 10); return; }
    char clean[512];
    snprintf(clean, sizeof clean, "%s", path);
    char *q = strchr(clean, '?'); if (q) *q = 0;
    snprintf(full, sizeof full, "%s%s", g_webroot, strcmp(clean, "/") == 0 ? "/index.html" : clean);
    int f = open(full, O_RDONLY);
    if (f < 0 || fstat(f, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (f >= 0) close(f);
        reply(fd, 404, "text/plain", "not found\n", 10);
        return;
    }
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                     CORS "Connection: close\r\n\r\n", mime(full), (long long)st.st_size);
    send_all(fd, h, (size_t)n);
    char buf[8192];
    ssize_t r;
    while ((r = read(f, buf, sizeof buf)) > 0)
        if (send_all(fd, buf, (size_t)r) != 0) break;
    close(f);
}

static void stream(int fd)
{
    static const char h[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                            "Cache-Control: no-store\r\n" CORS "Connection: keep-alive\r\n\r\n";
    char buf[8192];
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (send_all(fd, h, sizeof h - 1) != 0) return;
    for (;;) {
        memcpy(buf, "data: ", 6);
        int n = 6 + cc_snapshot_json(buf + 6, (int)sizeof buf - 10);
        memcpy(buf + n, "\n\n", 2);
        if (send_all(fd, buf, (size_t)n + 2) != 0) return;
        struct timespec ts = { 0, 100000000L };
        nanosleep(&ts, NULL);
    }
}

static int qparam(const char *path, const char *key, char *out, int outlen)
{
    const char *q = strchr(path, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            q += kl + 1;
            int i = 0;
            while (*q && *q != '&' && *q != ' ' && i < outlen - 1) out[i++] = *q++;
            out[i] = 0;
            return 1;
        }
        q = strchr(q, '&');
    }
    return 0;
}

static void *conn(void *arg)
{
    int fd = (int)(long)arg;
    char req[4096], method[8], path[512];
    int n = 0;

    while (n < (int)sizeof req - 1) {
        ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) break;
        n += (int)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[n] = 0;
    if (sscanf(req, "%7s %511s", method, path) != 2) { close(fd); return NULL; }

    if (!strcmp(method, "OPTIONS")) {
        reply(fd, 204, "text/plain", "", 0);
    } else if (!strncmp(path, "/api/stream", 11)) {
        stream(fd);
    } else if (!strncmp(path, "/api/state", 10)) {
        char buf[8192];
        int l = cc_snapshot_json(buf, sizeof buf);
        reply(fd, 200, "application/json", buf, (size_t)l);
    } else if (!strncmp(path, "/api/cmd", 8)) {
        char cmd[48], r[256], body[400];
        if (!qparam(path, "do", cmd, sizeof cmd)) {
            reply(fd, 400, "application/json", "{\"ok\":false}", 12);
        } else {
            int ok = cc_command(cmd, r, sizeof r) == 0;
            char *nl = strchr(r, '\n'); if (nl) *nl = 0;
            int l = snprintf(body, sizeof body, "{\"ok\":%s,\"reply\":\"%s\"}", ok ? "true" : "false", r);
            reply(fd, ok ? 200 : 400, "application/json", body, (size_t)l);
        }
    } else if (!strcmp(method, "GET")) {
        serve_file(fd, path);
    } else {
        reply(fd, 404, "text/plain", "not found\n", 10);
    }
    close(fd);
    return NULL;
}

static int g_lfd;

static void *acceptor(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(g_lfd, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        pthread_t t;
        if (cc_thread(&t, CC_PRIO_HTTP, SCHED_RR, conn, (void *)(long)c) != 0) close(c);
    }
    return NULL;
}

int cc_http_start(int port, const char *webroot)
{
    struct sockaddr_in a;
    int one = 1;
    pthread_t t;

    if (webroot) snprintf(g_webroot, sizeof g_webroot, "%s", webroot);
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_lfd < 0) { perror("socket"); return -1; }
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_lfd, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return -1; }
    if (listen(g_lfd, 16) != 0) { perror("listen"); return -1; }
    cc_thread(&t, CC_PRIO_HTTP, SCHED_RR, acceptor, NULL);
    cc_event('i', "dashboard API on :%d%s%s", port, g_webroot[0] ? ", web root " : "", g_webroot);
    return 0;
}
