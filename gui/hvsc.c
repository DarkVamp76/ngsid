/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "hvsc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static const char *const MIRRORS[] = {
    "https://hvsc.brona.dk/HVSC/C64Music/",
    "http://hafnium.prg.dtu.dk/HVSC/C64Music/",
};
#define NMIRRORS ((int)(sizeof MIRRORS / sizeof *MIRRORS))
#define MAX_BODY (8u * 1024 * 1024)

static const char *mirror_base(int i) {
    const char *e = getenv("NGSID_HVSC_MIRROR");
    if (e && *e) return i == 0 ? e : NULL;
    return i < NMIRRORS ? MIRRORS[i] : NULL;
}

/* ---------------------------------------------------------------- path helpers */

static int safe_path(const char *p) {
    if (p[0] == '/') return 0;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == 2 && s[0] == '.' && s[1] == '.') return 0;
        if (!e) break;
        s = e + 1;
    }
    return 1;
}

static void url_encode(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') out[o++] = (char)c;
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
    }
    out[o] = 0;
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (s[i] == '%' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            out[o++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        } else
            out[o++] = s[i];
    }
    out[o] = 0;
}

static int mkdir_p(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(path, 0755) && errno != EEXIST) { *p = '/'; return -1; }
        *p = '/';
    }
    return 0;
}

/* ---------------------------------------------------------------- curl */

/* Run curl. With outfile the body goes to that file, otherwise into a malloc'd buffer. */
static int run_curl(const char *url, const char *outfile, char **buf, size_t *len) {
    int pfd[2] = { -1, -1 };
    char *argv[20];
    int k = 0;
    argv[k++] = "curl"; argv[k++] = "-fsSL";
    argv[k++] = "--max-time"; argv[k++] = "25";
    argv[k++] = "--connect-timeout"; argv[k++] = "8";
    argv[k++] = "--max-filesize"; argv[k++] = "8000000";
    argv[k++] = "-A"; argv[k++] = "ngSid (+https://github.com/DarkVamp76/ngsid)";
    if (outfile) { argv[k++] = "-o"; argv[k++] = (char *)outfile; }
    argv[k++] = "--"; argv[k++] = (char *)url; argv[k] = NULL;

    if (!outfile && pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        if (!outfile) { close(pfd[0]); close(pfd[1]); }
        return -1;
    }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); }
        if (!outfile) { dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]); }
        execvp("curl", argv);
        _exit(127);
    }
    int rc = 0;
    if (!outfile) {
        close(pfd[1]);
        size_t cap = 65536, n = 0;
        char *b = (char *)malloc(cap);
        for (;;) {
            if (n + 4096 > cap) {
                if (cap >= MAX_BODY) { rc = -1; break; }
                cap *= 2;
                b = (char *)realloc(b, cap);
            }
            ssize_t r = read(pfd[0], b + n, cap - n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += (size_t)r;
        }
        close(pfd[0]);
        *buf = b;
        *len = n;
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) { if (!outfile) { free(*buf); *buf = NULL; } return HVSC_ERR_NOCURL; }
    if (rc || !WIFEXITED(st) || WEXITSTATUS(st) != 0) { if (!outfile) { free(*buf); *buf = NULL; } return -1; }
    return 0;
}


#ifdef __EMSCRIPTEN__
/* Browser: no fork/curl. A synchronous XMLHttpRequest asks the same-origin PHP proxy (hvsc.php) and the
 * answer is written into the in-memory file system, from where the normal C code reads it. */
static int web_get(const char *kind, const char *path, const char *outfile) {
    char enc[1100];
    url_encode(path, enc, sizeof enc);
    /* note: no top-level commas in the JS body (the C preprocessor would split the macro arguments there) */
    return EM_ASM_INT({
        function str(p) { var r = ''; for (var c; (c = HEAPU8[p++]);) r += String.fromCharCode(c); return r; }
        try {
            var kind = str($0);
            var path = decodeURIComponent(str($1));
            var out = decodeURIComponent(escape(str($2)));
            var x = new XMLHttpRequest();
            x.open('GET', (window.NGWEB_HVSC_URL || 'hvsc.php') + '?' + kind + '=' + encodeURIComponent(path), false);
            x.overrideMimeType('text/plain; charset=x-user-defined');
            x.send(null);
            if (x.status !== 200) return -1;
            var s = x.responseText;
            var n = s.length;
            var b = new Uint8Array(n);
            for (var i = 0; i < n; i++) b[i] = s.charCodeAt(i) & 255;
            var d = out.substring(0, out.lastIndexOf('/'));
            if (d) FS.mkdirTree(d);
            FS.writeFile(out, b);
            return n;
        } catch (e) { return -1; }
    }, kind, enc, outfile);
}
#endif

/* ---------------------------------------------------------------- listing */

static int cmp_entry(const void *a, const void *b) {
    const hvsc_entry *x = (const hvsc_entry *)a, *y = (const hvsc_entry *)b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

int hvsc_parse_listing(const char *html, size_t len, hvsc_entry **out, int *n) {
    hvsc_entry *e = NULL;
    int cnt = 0, cap = 0;
    const char *p = html, *end = html + len;
    static const char key[] = "<a href=\"";
    while (p < end) {
        const char *a = NULL;
        for (const char *q = p; q + sizeof key - 1 < end; q++)
            if (!strncasecmp(q, key, sizeof key - 1)) { a = q + sizeof key - 1; break; }
        if (!a) break;
        const char *z = memchr(a, '"', (size_t)(end - a));
        if (!z) break;
        p = z + 1;
        size_t hl = (size_t)(z - a);
        if (!hl || a[0] == '?' || a[0] == '/' || a[0] == '#' || memchr(a, ':', hl)) continue;
        char name[256];
        url_decode(a, hl, name, sizeof name);
        size_t nl = strlen(name);
        int dir = nl && name[nl - 1] == '/';
        if (dir) name[--nl] = 0;
        if (!nl || name[0] == '.' || strchr(name, '/')) continue;
        if (!dir && !(nl > 4 && !strcasecmp(name + nl - 4, ".sid"))) continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 128;
            e = (hvsc_entry *)realloc(e, (size_t)cap * sizeof *e);
        }
        snprintf(e[cnt].name, sizeof e[cnt].name, "%s", name);
        e[cnt].dir = dir;
        cnt++;
    }
    if (cnt) qsort(e, (size_t)cnt, sizeof *e, cmp_entry);
    *out = e;
    *n = cnt;
    return 0;
}

int hvsc_http_get(const char *url, char **buf, size_t *len) {
    *buf = NULL; *len = 0;
#ifdef __EMSCRIPTEN__
    (void)url;
    return -1;
#endif
    return run_curl(url, NULL, buf, len);
}

int hvsc_list(const char *folder, hvsc_entry **out, int *n) {
    if (!safe_path(folder)) return -1;
#ifdef __EMSCRIPTEN__
    {
        const char *tmp = "/tmp/ngsid-hvsc-list.html";
        if (web_get("list", folder, tmp) < 0) return -1;
        FILE *f = fopen(tmp, "rb");
        if (!f) return -1;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || (size_t)sz > MAX_BODY) { fclose(f); return -1; }
        char *buf = (char *)malloc((size_t)sz);
        size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0;
        fclose(f);
        unlink(tmp);
        if (!buf || got != (size_t)sz) { free(buf); return -1; }
        int rc = hvsc_parse_listing(buf, got, out, n);
        free(buf);
        return rc;
    }
#endif
    char enc[1024];
    url_encode(folder, enc, sizeof enc);
    int rc = -1;
    for (int i = 0; mirror_base(i); i++) {
        char url[1400];
        snprintf(url, sizeof url, "%s%s", mirror_base(i), enc);
        char *buf = NULL;
        size_t len = 0;
        rc = run_curl(url, NULL, &buf, &len);
        if (rc == HVSC_ERR_NOCURL) return rc;
        if (rc == 0) {
            rc = hvsc_parse_listing(buf, len, out, n);
            free(buf);
            return rc;
        }
    }
    return -1;
}


int hvsc_resolve_id(int id, char *path, size_t cap) {
#ifdef __EMSCRIPTEN__
    char num[16];
    snprintf(num, sizeof num, "%d", id);
    const char *tmp = "/tmp/ngsid-hvsc-id.txt";
    if (id <= 0 || web_get("id", num, tmp) < 0) return -1;
    FILE *f = fopen(tmp, "rb");
    if (!f) return -1;
    size_t n = fread(path, 1, cap - 1, f);
    fclose(f);
    unlink(tmp);
    path[n] = 0;
    while (n && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' ')) path[--n] = 0;
    return (n && safe_path(path)) ? 0 : -1;
#else
    (void)id; (void)path; (void)cap;
    return -1;
#endif
}

/* ---------------------------------------------------------------- download */

int hvsc_fetch(const char *file, char *cache_path, size_t cap) {
    if (!safe_path(file) || !*file) return -1;
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char base[512];
    if (xdg && *xdg) snprintf(base, sizeof base, "%s/ngsid/hvsc", xdg);
    else if (home && *home) snprintf(base, sizeof base, "%s/.cache/ngsid/hvsc", home);
    else snprintf(base, sizeof base, "/tmp/ngsid-cache/hvsc");
    if ((size_t)snprintf(cache_path, cap, "%s/%s", base, file) >= cap) return -1;
    struct stat st;
    if (!stat(cache_path, &st) && st.st_size > 0) return 0;
#ifdef __EMSCRIPTEN__
    return web_get("file", file, cache_path) > 0 ? 0 : -1;
#endif
    char dirs[1100];
    snprintf(dirs, sizeof dirs, "%s", cache_path);
    if (mkdir_p(dirs)) return -1;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.part", cache_path);
    char enc[1024];
    url_encode(file, enc, sizeof enc);
    int rc = -1;
    for (int i = 0; mirror_base(i); i++) {
        char url[1400];
        snprintf(url, sizeof url, "%s%s", mirror_base(i), enc);
        rc = run_curl(url, tmp, NULL, NULL);
        if (rc == HVSC_ERR_NOCURL) return rc;
        if (rc == 0) break;
    }
    if (rc == 0 && (stat(tmp, &st) || st.st_size <= 0 || rename(tmp, cache_path))) rc = -1;
    if (rc) unlink(tmp);
    return rc;
}
