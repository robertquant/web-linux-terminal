/*
 * httpd - Tiny Terminal HTTP Server with Live Dashboard
 *
 * A minimal HTTP/1.1 server written in pure C with a curses-based
 * real-time monitoring dashboard. Serves static files from a
 * configurable directory and displays live request logs, response
 * codes, bandwidth, and connection stats.
 *
 * Features:
 *   - HTTP/1.1 GET / HEAD support
 *   - MIME type detection for 30+ file types
 *   - Directory listing with clickable-style formatting
 *   - Live ncurses dashboard: request log, stats, bandwidth meter
 *   - configurable port and document root
 *   - ETag / If-None-Match for cache validation (304)
 *   - Range requests (partial content 206)
 *   - concurrent connections via fork()
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra -std=c11 -o httpd httpd-4295.c -lncurses -lpthread
 *
 * Run:
 *   ./httpd                         # port 8080, serve ./www
 *   ./httpd -p 3000 -d /var/www    # port 3000, serve /var/www
 *
 * Press 'q' in the dashboard to quit.
 *
 * Author: Claude   License: MIT
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <pthread.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

/* ── Configuration ─────────────────────────────────────── */

#define DEFAULT_PORT    8080
#define DEFAULT_ROOT    "./www"
#define MAX_REQUEST     8192
#define LOG_LINES       500
#define VISIBLE_LOG     18
#define MAX_PATH_LEN    4096
#define MAX_HEADER      128

/* ── Data Structures ───────────────────────────────────── */

typedef struct {
    char method[8];
    char path[MAX_PATH_LEN];
    char version[16];
    char host[256];
    char if_none_match[64];
    char range[128];
    int  head_only;
} HttpRequest;

typedef struct {
    time_t ts;
    char   client[48];
    char   method[8];
    char   path[256];
    int    status;
    long   bytes;
    char   content_type[64];
} LogEntry;

typedef struct {
    long total_requests;
    long total_bytes;
    long status_2xx;
    long status_3xx;
    long status_4xx;
    long status_5xx;
    long active_conns;
    long get_count;
    long head_count;
} Stats;

/* ── Globals ───────────────────────────────────────────── */

static int       g_port        = DEFAULT_PORT;
static char      g_root[MAX_PATH_LEN] = DEFAULT_ROOT;
static int       g_server_fd   = -1;
static volatile int g_running  = 1;
static int       g_log_scroll  = 0;

static LogEntry  g_log[LOG_LINES];
static int       g_log_count   = 0;
static Stats     g_stats       = {0};
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── MIME Type Table ───────────────────────────────────── */

typedef struct { const char *ext; const char *mime; } MimeMap;

static const MimeMap mime_types[] = {
    {"html","text/html"},       {"htm","text/html"},
    {"css","text/css"},         {"js","application/javascript"},
    {"json","application/json"},{"xml","application/xml"},
    {"txt","text/plain"},       {"md","text/markdown"},
    {"csv","text/csv"},         {"log","text/plain"},
    {"png","image/png"},        {"jpg","image/jpeg"},
    {"jpeg","image/jpeg"},     {"gif","image/gif"},
    {"svg","image/svg+xml"},    {"ico","image/x-icon"},
    {"webp","image/webp"},      {"bmp","image/bmp"},
    {"pdf","application/pdf"},  {"zip","application/zip"},
    {"gz","application/gzip"},  {"tar","application/x-tar"},
    {"mp3","audio/mpeg"},       {"wav","audio/wav"},
    {"ogg","audio/ogg"},        {"mp4","video/mp4"},
    {"webm","video/webm"},      {"avi","video/x-msvideo"},
    {"woff","font/woff"},       {"woff2","font/woff2"},
    {"ttf","font/ttf"},         {"otf","font/otf"},
    {"bin","application/octet-stream"},
    {"exe","application/octet-stream"},
    {"sh","text/x-shellscript"},
    {"py","text/x-python"},
    {"c","text/x-c"},           {"h","text/x-c"},
    {"java","text/x-java-source"},
    {NULL, NULL}
};

static const char *get_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; mime_types[i].ext; i++) {
        if (strcasecmp(dot + 1, mime_types[i].ext) == 0)
            return mime_types[i].mime;
    }
    return "application/octet-stream";
}

/* ── Utility ───────────────────────────────────────────── */

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static void url_decode(char *dst, const char *src, size_t dstlen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstlen - 1; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i+1])
                           && isxdigit((unsigned char)src[i+2])) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static void format_size(char *buf, size_t sz) {
    if (sz < 1024)            sprintf(buf, "%lu B", (unsigned long)sz);
    else if (sz < 1048576)    sprintf(buf, "%.1f KB", sz / 1024.0);
    else if (sz < 1073741824) sprintf(buf, "%.1f MB", sz / 1048576.0);
    else                      sprintf(buf, "%.2f GB", sz / 1073741824.0);
}

static void compute_etag(char *etag, size_t size, time_t mtime) {
    sprintf(etag, "\"%lx-%lx\"", (unsigned long)mtime, (unsigned long)size);
}

/* ── Logging ───────────────────────────────────────────── */

static void add_log(const char *client, const char *method,
                    const char *path, int status, long bytes,
                    const char *ctype) {
    pthread_mutex_lock(&g_mutex);
    int idx = g_log_count % LOG_LINES;
    g_log[idx].ts = time(NULL);
    strncpy(g_log[idx].client, client, sizeof(g_log[idx].client) - 1);
    strncpy(g_log[idx].method, method, sizeof(g_log[idx].method) - 1);
    strncpy(g_log[idx].path, path, sizeof(g_log[idx].path) - 1);
    g_log[idx].status = status;
    g_log[idx].bytes = bytes;
    strncpy(g_log[idx].content_type, ctype, sizeof(g_log[idx].content_type) - 1);
    g_log_count++;

    g_stats.total_requests++;
    g_stats.total_bytes += bytes;
    if (status >= 200 && status < 300) g_stats.status_2xx++;
    else if (status >= 300 && status < 400) g_stats.status_3xx++;
    else if (status >= 400 && status < 500) g_stats.status_4xx++;
    else g_stats.status_5xx++;
    if (strcmp(method, "GET") == 0)  g_stats.get_count++;
    if (strcmp(method, "HEAD") == 0) g_stats.head_count++;
    pthread_mutex_unlock(&g_mutex);
}

/* ── HTTP Parsing ──────────────────────────────────────── */

static int parse_request(int fd, HttpRequest *req) {
    char buf[MAX_REQUEST];
    int total = 0, n;
    /* Read until we get \r\n\r\n or buffer fills */
    while (total < (int)sizeof(buf) - 1) {
        n = recv(fd, buf + total, 1, 0);
        if (n <= 0) return -1;
        total += n;
        buf[total] = '\0';
        if (total >= 4 && memcmp(buf + total - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    if (total < 16) return -1;

    /* Parse request line */
    char *savep = NULL;
    char *line = strtok_r(buf, "\r\n", &savep);
    if (!line) return -1;
    if (sscanf(line, "%7s %4095s %15s", req->method, req->path, req->version) != 3)
        return -1;

    if (strcmp(req->method, "HEAD") == 0) req->head_only = 1;
    else if (strcmp(req->method, "GET") != 0) return -2; /* method not allowed */

    /* Decode URL path, strip query string */
    char decoded[MAX_PATH_LEN];
    char *qmark = strchr(req->path, '?');
    if (qmark) *qmark = '\0';
    url_decode(decoded, req->path, sizeof(decoded));
    strncpy(req->path, decoded, sizeof(req->path) - 1);

    /* Parse headers we care about */
    while ((line = strtok_r(NULL, "\r\n", &savep)) != NULL) {
        if (strncasecmp(line, "Host:", 5) == 0) {
            char *v = line + 5; while (*v == ' ') v++;
            strncpy(req->host, v, sizeof(req->host) - 1);
        } else if (strncasecmp(line, "If-None-Match:", 14) == 0) {
            char *v = line + 14; while (*v == ' ') v++;
            strncpy(req->if_none_match, v, sizeof(req->if_none_match) - 1);
        } else if (strncasecmp(line, "Range:", 6) == 0) {
            char *v = line + 6; while (*v == ' ') v++;
            strncpy(req->range, v, sizeof(req->range) - 1);
        }
    }
    return 0;
}

/* ── Response Helpers ──────────────────────────────────── */

static void send_response(int fd, int status, const char *ctype,
                          const char *body, long body_len,
                          const char *extra_headers) {
    char header[2048];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: httpd-tiny/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        status, status_text(status), ctype, body_len,
        extra_headers ? extra_headers : "");
    send(fd, header, hlen, 0);
    if (body && body_len > 0)
        send(fd, body, body_len, 0);
}

static void send_file_response(int fd, const char *fpath,
                               struct stat *st, HttpRequest *req) {
    const char *ctype = get_mime(fpath);
    char etag[64];
    compute_etag(etag, st->st_size, st->st_mtime);

    /* Conditional GET via ETag */
    if (req->if_none_match[0] && strcmp(req->if_none_match, etag) == 0) {
        char h[256];
        snprintf(h, sizeof(h), "ETag: %s\r\n", etag);
        send_response(fd, 304, ctype, NULL, 0, h);
        return;
    }

    /* Range request parsing: bytes=start-end */
    long range_start = -1, range_end = -1;
    if (req->range[0] && strncmp(req->range, "bytes=", 6) == 0) {
        char *rs = (char *)(req->range + 6);
        char *dash = strchr(rs, '-');
        if (dash) {
            *dash = '\0';
            range_start = atol(rs);
            range_end = (dash[1]) ? atol(dash + 1) : (long)st->st_size - 1;
            if (range_start > range_end || range_end >= st->st_size) {
                send_response(fd, 416, "text/plain", "Range Not Satisfiable", 22, NULL);
                return;
            }
        }
    }

    FILE *fp = fopen(fpath, "rb");
    if (!fp) { send_response(fd, 500, "text/plain", "Internal Error", 14, NULL); return; }

    int status;
    long content_len;
    char extra[512] = {0};
    char etag_hdr[128];
    snprintf(etag_hdr, sizeof(etag_hdr), "ETag: %s\r\n", etag);

    if (range_start >= 0) {
        status = 206;
        content_len = range_end - range_start + 1;
        snprintf(extra, sizeof(extra),
            "%s"
            "Content-Range: bytes %ld-%ld/%ld\r\n"
            "Accept-Ranges: bytes\r\n",
            etag_hdr, range_start, range_end, (long)st->st_size);
        fseek(fp, range_start, SEEK_SET);
    } else {
        status = 200;
        content_len = st->st_size;
        snprintf(extra, sizeof(extra),
            "%s"
            "Accept-Ranges: bytes\r\n"
            "Last-Modified: ...\r\n",
            etag_hdr);
    }

    /* Send header */
    char header[2048];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: httpd-tiny/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        status, status_text(status), ctype, content_len, extra);
    send(fd, header, hlen, 0);

    /* Send body (unless HEAD) */
    if (!req->head_only) {
        char iobuf[16384];
        long remaining = content_len;
        while (remaining > 0) {
            size_t toread = (remaining > (long)sizeof(iobuf)) ? sizeof(iobuf) : (size_t)remaining;
            size_t got = fread(iobuf, 1, toread, fp);
            if (got == 0) break;
            send(fd, iobuf, got, 0);
            remaining -= got;
        }
    }
    fclose(fp);
}

/* ── Directory Listing ─────────────────────────────────── */

static void send_dir_listing(int fd, const char *dirpath,
                             const char *urlpath, HttpRequest *req) {
    DIR *d = opendir(dirpath);
    if (!d) { send_response(fd, 500, "text/plain", "Cannot open directory", 22, NULL); return; }

    /* Build HTML in memory */
    char *html = malloc(65536);
    if (!html) { closedir(d); return; }
    int pos = 0;

    pos += snprintf(html + pos, 65536 - pos,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Index of %s</title>"
        "<style>"
        "body{font-family:'Courier New',monospace;background:#1a1a2e;color:#e0e0e0;padding:20px}"
        "h1{color:#e94560;border-bottom:2px solid #e94560;padding-bottom:10px}"
        "table{width:100%%;border-collapse:collapse;margin-top:20px}"
        "th{text-align:left;background:#16213e;padding:10px;color:#0f3460}"
        "th:first-child{text-align:left}"
        "td{padding:8px 10px;border-bottom:1px solid #16213e}"
        "a{color:#53a8b6;text-decoration:none}a:hover{text-decoration:underline}"
        ".dir{color:#e94560;font-weight:bold}"
        ".size{color:#888}"
        "</style></head><body>"
        "<h1>Index of %s</h1><table>"
        "<tr><th>Name</th><th>Size</th><th>Modified</th></tr>",
        urlpath, urlpath);

    /* Parent directory link */
    if (strcmp(urlpath, "/") != 0) {
        char parent[MAX_PATH_LEN];
        strncpy(parent, urlpath, sizeof(parent) - 1);
        char *last = strrchr(parent, '/');
        if (last && last != parent) *last = '\0';
        else strcpy(parent, "/");
        pos += snprintf(html + pos, 65536 - pos,
            "<tr><td><a href='%s' class='dir'>../</a></td><td>-</td><td>-</td></tr>",
            parent);
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && ent->d_name[1] == '\0') continue;
        if (ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;

        char fullpath[MAX_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M",
                 localtime(&st.st_mtime));

        char link[MAX_PATH_LEN];
        if (urlpath[strlen(urlpath)-1] == '/')
            snprintf(link, sizeof(link), "%s%s", urlpath, ent->d_name);
        else
            snprintf(link, sizeof(link), "%s/%s", urlpath, ent->d_name);

        if (S_ISDIR(st.st_mode)) {
            pos += snprintf(html + pos, 65536 - pos,
                "<tr><td><a href='%s/' class='dir'>%s/</a></td>"
                "<td>-</td><td>%s</td></tr>",
                link, ent->d_name, timebuf);
        } else {
            char szbuf[32];
            format_size(szbuf, st.st_size);
            pos += snprintf(html + pos, 65536 - pos,
                "<tr><td><a href='%s'>%s</a></td>"
                "<td class='size'>%s</td><td>%s</td></tr>",
                link, ent->d_name, szbuf, timebuf);
        }
    }
    closedir(d);

    pos += snprintf(html + pos, 65536 - pos,
        "</table><hr><p style='color:#555;font-size:12px'>"
        "httpd-tiny/1.0 &mdash; %s</p></body></html>",
        req->host[0] ? req->host : "localhost");

    send_response(fd, 200, "text/html; charset=utf-8", html, pos, NULL);
    free(html);
}

/* ── Request Handler ───────────────────────────────────── */

static void handle_client(int fd, struct sockaddr_in *addr) {
    char client_ip[48];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));

    HttpRequest req = {0};
    int rc = parse_request(fd, &req);

    if (rc == -2) {
        send_response(fd, 405, "text/plain", "Method Not Allowed", 18,
                      "Allow: GET, HEAD\r\n");
        add_log(client_ip, req.method[0] ? req.method : "???", req.path[0] ? req.path : "/",
                405, 0, "text/plain");
        close(fd);
        return;
    }
    if (rc < 0) {
        send_response(fd, 400, "text/plain", "Bad Request", 11, NULL);
        add_log(client_ip, "???", "/", 400, 0, "text/plain");
        close(fd);
        return;
    }

    /* Security: reject path traversal */
    if (strstr(req.path, "..")) {
        send_response(fd, 403, "text/plain", "Forbidden", 9, NULL);
        add_log(client_ip, req.method, req.path, 403, 0, "text/plain");
        close(fd);
        return;
    }

    /* Map URL path to filesystem */
    char fpath[MAX_PATH_LEN * 2];
    if (strcmp(req.path, "/") == 0)
        snprintf(fpath, sizeof(fpath), "%s/index.html", g_root);
    else
        snprintf(fpath, sizeof(fpath), "%s%s", g_root, req.path);

    struct stat st;
    if (stat(fpath, &st) != 0) {
        /* Try serving directory listing */
        if (errno == ENOENT || errno == ENOTDIR) {
            /* Maybe it's a directory without trailing slash */
            if (stat(fpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                /* Redirect to add trailing slash */
                char redir[MAX_PATH_LEN + 32];
                snprintf(redir, sizeof(redir), "Location: %s/\r\n", req.path);
                send_response(fd, 301, "text/html", "", 0, redir);
                add_log(client_ip, req.method, req.path, 301, 0, "text/html");
                close(fd);
                return;
            }
        }
        /* 404 - send a styled error page */
        const char *err_html =
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<style>body{background:#1a1a2e;color:#e94560;font-family:monospace;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
            "h1{font-size:72px;margin:0}p{color:#888;font-size:18px}</style></head>"
            "<body><div style='text-align:center'>"
            "<h1>404</h1><p>The requested resource was not found.</p>"
            "</div></body></html>";
        send_response(fd, 404, "text/html", err_html, strlen(err_html), NULL);
        add_log(client_ip, req.method, req.path, 404, strlen(err_html), "text/html");
        close(fd);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        /* Check for index.html inside directory */
        char idx[MAX_PATH_LEN * 2];
        snprintf(idx, sizeof(idx), "%s/index.html", fpath);
        if (stat(idx, &st) == 0 && S_ISREG(st.st_mode)) {
            send_file_response(fd, idx, &st, &req);
            add_log(client_ip, req.method, req.path, 200, st.st_size, get_mime(idx));
        } else {
            send_dir_listing(fd, fpath, req.path, &req);
            add_log(client_ip, req.method, req.path, 200, 0, "text/html");
        }
    } else if (S_ISREG(st.st_mode)) {
        long before = st.st_size;
        send_file_response(fd, fpath, &st, &req);
        add_log(client_ip, req.method, req.path, 200, before, get_mime(fpath));
    } else {
        send_response(fd, 403, "text/plain", "Forbidden", 9, NULL);
        add_log(client_ip, req.method, req.path, 403, 0, "text/plain");
    }

    close(fd);
}

/* ── Dashboard (ncurses) ───────────────────────────────── */

#ifdef __has_include
#if __has_include(<ncurses.h>)
#include <ncurses.h>
#define HAS_NCURSES 1
#else
#define HAS_NCURSES 0
#endif
#else
#include <ncurses.h>
#define HAS_NCURSES 1
#endif

#if HAS_NCURSES

static void draw_dashboard(void) {
    clear();

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)rows;

    /* Title bar */
    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(0, 0, "  httpd-tiny/1.0  |  Port: %-5d  |  Root: %-30s  |  [q] Quit  ",
             g_port, g_root);
    attroff(A_BOLD | COLOR_PAIR(1));

    /* Separator */
    attron(COLOR_PAIR(2));
    mvhline(1, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(2));

    /* Request log header */
    attron(A_BOLD);
    mvprintw(2, 0, " %-15s %-6s %-40s %-6s %-10s %-20s",
             "Client", "Method", "Path", "Status", "Size", "Content-Type");
    attroff(A_BOLD);

    /* Separator */
    attron(COLOR_PAIR(2));
    mvhline(3, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(2));

    /* Log entries (newest first) */
    pthread_mutex_lock(&g_mutex);
    int start = g_log_count - VISIBLE_LOG - g_log_scroll;
    if (start < 0) start = 0;
    int end = g_log_count - g_log_scroll;
    if (end > g_log_count) end = g_log_count;

    for (int i = end - 1, row = 4; i >= start && row < rows - 10; i--, row++) {
        int idx = ((i % LOG_LINES) + LOG_LINES) % LOG_LINES;
        LogEntry *e = &g_log[idx];

        int color = 3; /* green for 2xx */
        if (e->status >= 300 && e->status < 400) color = 4; /* yellow */
        else if (e->status >= 400) color = 5;                /* red */

        char timebuf[16];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime(&e->ts));

        char szbuf[16];
        format_size(szbuf, e->bytes);

        attron(COLOR_PAIR(color));
        mvprintw(row, 0, " %s %-15s %-6s %-40s %d %-4s %-10s %-20s",
                 timebuf, e->client, e->method, e->path,
                 e->status, status_text(e->status), szbuf, e->content_type);
        attroff(COLOR_PAIR(color));
    }

    /* Stats separator */
    int stat_row = rows - 9;
    attron(COLOR_PAIR(2));
    mvhline(stat_row, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(2));

    /* Stats panel */
    attron(A_BOLD);
    mvprintw(stat_row + 1, 2, "STATISTICS");
    attroff(A_BOLD);

    Stats snap = g_stats;
    pthread_mutex_unlock(&g_mutex);

    char bw_str[32];
    format_size(bw_str, snap.total_bytes);
    mvprintw(stat_row + 2, 2,
        "Total Requests: %-8ld  |  GET: %-6ld  HEAD: %-6ld  |  Bandwidth: %s",
        snap.total_requests, snap.get_count, snap.head_count, bw_str);

    /* Status code breakdown */
    mvprintw(stat_row + 3, 2, "Status:  ");
    attron(COLOR_PAIR(3));
    printw("2xx: %-6ld  ", snap.status_2xx);
    attroff(COLOR_PAIR(3));
    attron(COLOR_PAIR(4));
    printw("3xx: %-6ld  ", snap.status_3xx);
    attroff(COLOR_PAIR(4));
    attron(COLOR_PAIR(5));
    printw("4xx: %-6ld  ", snap.status_4xx);
    attroff(COLOR_PAIR(5));
    attron(COLOR_PAIR(5));
    printw("5xx: %-6ld", snap.status_5xx);
    attroff(COLOR_PAIR(5));

    /* Bandwidth bar */
    int bar_row = stat_row + 5;
    mvprintw(bar_row, 2, "Bandwidth:");
    long bw_bar_len = cols - 30;
    if (bw_bar_len > 100) bw_bar_len = 100;
    if (bw_bar_len < 10) bw_bar_len = 10;
    attron(COLOR_PAIR(3));
    long fill = snap.total_bytes / 100000;
    if (fill > bw_bar_len) fill = bw_bar_len;
    for (long i = 0; i < fill; i++) addch(ACS_CKBOARD);
    attroff(COLOR_PAIR(3));
    for (long i = fill; i < bw_bar_len; i++) addch(' ');

    /* Footer */
    mvprintw(rows - 1, 0,
        " Active: %ld  |  Up: %lds  |  Scroll: PgUp/PgDn  |  [q] Quit  ",
        snap.active_conns, (long)(time(NULL) - snap.total_requests > 0 ? time(NULL) : time(NULL)));

    refresh();
}

static void *dashboard_thread(void *arg) {
    (void)arg;
    initscr();
    noecho();
    cbreak();
    nodelay(stdscr, TRUE);
    curs_set(0);
    keypad(stdscr, TRUE);

    start_color();
    init_pair(1, COLOR_WHITE, COLOR_BLUE);    /* title bar */
    init_pair(2, COLOR_CYAN, COLOR_BLACK);    /* separators */
    init_pair(3, COLOR_GREEN, COLOR_BLACK);   /* 2xx / ok */
    init_pair(4, COLOR_YELLOW, COLOR_BLACK);  /* 3xx */
    init_pair(5, COLOR_RED, COLOR_BLACK);     /* 4xx/5xx */

    while (g_running) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            g_running = 0;
            break;
        }
        if (ch == KEY_PPAGE) g_log_scroll += 5;
        if (ch == KEY_NPAGE) { g_log_scroll -= 5; if (g_log_scroll < 0) g_log_scroll = 0; }
        draw_dashboard();
        usleep(100000); /* 10 FPS */
    }

    endwin();
    return NULL;
}

#endif /* HAS_NCURSES */

/* ── Connection Acceptor ───────────────────────────────── */

static void *accept_thread(void *arg) {
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int cfd = accept(g_server_fd, (struct sockaddr *)&client_addr, &clen);
        if (cfd < 0) {
            if (!g_running) break;
            continue;
        }

        pthread_mutex_lock(&g_mutex);
        g_stats.active_conns++;
        pthread_mutex_unlock(&g_mutex);

        /* Handle in a child process for concurrency */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child - close server socket, handle request */
            close(g_server_fd);
            handle_client(cfd, &client_addr);
            _exit(0);
        } else if (pid > 0) {
            /* Parent */
            close(cfd);
            /* Reap children periodically */
            while (waitpid(-1, NULL, WNOHANG) > 0);
        }

        pthread_mutex_lock(&g_mutex);
        g_stats.active_conns--;
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

/* ── Signal Handling ───────────────────────────────────── */

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
    }
}

/* ── Create Default www Directory ──────────────────────── */

static void create_sample_site(void) {
    struct stat st;
    if (stat(g_root, &st) == 0 && S_ISDIR(st.st_mode))
        return; /* already exists */

    mkdir(g_root, 0755);

    /* index.html */
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/index.html", g_root);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f,
"<!DOCTYPE html>\n"
"<html><head><meta charset='utf-8'>\n"
"<title>Welcome to httpd-tiny</title>\n"
"<style>\n"
"body{font-family:'Courier New',monospace;background:#0f0f23;color:#cccccc;"
"display:flex;align-items:center;justify-content:center;height:100vh;margin:0}\n"
".box{text-align:center;max-width:600px}\n"
"h1{color:#e94560;font-size:3em;margin:0}\n"
"h2{color:#53a8b6;font-weight:normal}\n"
"code{background:#1a1a2e;padding:8px 16px;border-radius:4px;color:#0f3460}\n"
"a{color:#e94560}\n"
"</style></head>\n"
"<body><div class='box'>\n"
"<h1>httpd-tiny</h1>\n"
"<h2>A tiny HTTP server written in pure C</h2>\n"
"<p>Features: GET/HEAD, MIME types, directory listing,<br>\n"
"ETag caching, range requests, live ncurses dashboard</p>\n"
"<p>Try creating files in <code>%s/</code></p>\n"
"</div></body></html>\n", g_root);
        fclose(f);
    }

    /* demo.txt */
    snprintf(path, sizeof(path), "%s/demo.txt", g_root);
    f = fopen(path, "w");
    if (f) {
        fprintf(f,
"Hello from httpd-tiny!\n"
"This is a sample text file.\n"
"Time: %s", ctime(&(time_t){time(NULL)}));
        fclose(f);
    }
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Parse command line */
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            g_port = atoi(argv[++i]);
            if (g_port < 1 || g_port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) && i + 1 < argc) {
            if (!realpath(argv[++i], g_root)) strncpy(g_root, argv[i], sizeof(g_root)-1);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("httpd-tiny/1.0 - A tiny HTTP server with live dashboard\n\n"
                   "Usage: %s [-p PORT] [-d DIR]\n\n"
                   "Options:\n"
                   "  -p, --port PORT   Port to listen on (default %d)\n"
                   "  -d, --dir  DIR    Document root (default %s)\n"
                   "  -h, --help        Show this help\n\n"
                   "Dashboard: press 'q' to quit\n",
                   argv[0], DEFAULT_PORT, DEFAULT_ROOT);
            return 0;
        }
    }

    /* Create sample site if needed */
    create_sample_site();

    /* Setup signal handlers */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN); /* auto-reap children */
    signal(SIGPIPE, SIG_IGN);

    /* Create socket */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(g_port)
    };

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_server_fd);
        return 1;
    }

    if (listen(g_server_fd, 64) < 0) {
        perror("listen");
        close(g_server_fd);
        return 1;
    }

    /* Launch threads */
    pthread_t accept_tid, dash_tid;

    pthread_create(&accept_tid, NULL, accept_thread, NULL);

#if HAS_NCURSES
    pthread_create(&dash_tid, NULL, dashboard_thread, NULL);
    pthread_join(dash_tid, NULL);
    /* Dashboard quit -> signal shutdown */
    g_running = 0;
    shutdown(g_server_fd, SHUT_RDWR);
#else
    printf("httpd-tiny/1.0 running on port %d, root=%s\n", g_port, g_root);
    printf("(ncurses not available, running in foreground. Ctrl+C to quit)\n");
    pthread_join(accept_tid, NULL);
#endif

    close(g_server_fd);
    pthread_join(accept_tid, NULL);

    return 0;
}
