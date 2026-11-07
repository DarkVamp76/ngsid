/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline test of the HVSC listing parser; with NGSID_HVSC_MIRROR set to a reachable test server
 * it also exercises hvsc_list / hvsc_fetch (see the Makefile target check-hvsc-net). */
#include "../gui/hvsc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    const char *html =
        "<html><body><h1>Index of /HVSC/C64Music</h1><table>"
        "<tr><th><a href=\"?C=N;O=D\">Name</a></th></tr>"
        "<tr><td><a href=\"/HVSC/\">Parent Directory</a></td></tr>"
        "<tr><td><a href=\"MUSICIANS/\">MUSICIANS/</a></td></tr>"
        "<tr><td><a href=\"DEMOS/\">DEMOS/</a></td></tr>"
        "<tr><td><a href=\"DOCUMENTS/\">DOCUMENTS/</a></td></tr>"
        "<tr><td><a href=\"Zeta%20Song.sid\">x</a></td></tr>"
        "<tr><td><a href=\"Arkanoid.SID\">x</a></td></tr>"
        "<tr><td><a href=\"readme.txt\">x</a></td></tr>"
        "<tr><td><a href=\"../evil/\">x</a></td></tr>"
        "<tr><td><a href=\"http://example.com/x.sid\">x</a></td></tr>"
        "<tr><td><a href=\"a%2Fb.sid\">x</a></td></tr>"
        "</table></body></html>";
    hvsc_entry *e = NULL;
    int n = 0;
    CHECK(hvsc_parse_listing(html, strlen(html), &e, &n) == 0);
    CHECK(n == 5);
    if (n == 5) {
        CHECK(e[0].dir && !strcmp(e[0].name, "DEMOS"));
        CHECK(e[1].dir && !strcmp(e[1].name, "DOCUMENTS"));
        CHECK(e[2].dir && !strcmp(e[2].name, "MUSICIANS"));
        CHECK(!e[3].dir && !strcmp(e[3].name, "Arkanoid.SID"));
        CHECK(!e[4].dir && !strcmp(e[4].name, "Zeta Song.sid"));
    }
    free(e);
    CHECK(hvsc_parse_listing("", 0, &e, &n) == 0 && n == 0);

    const char *mirror = getenv("NGSID_HVSC_MIRROR");
    if (mirror && *mirror) {
        hvsc_entry *l = NULL;
        int ln = 0;
        int rc = hvsc_list("", &l, &ln);
        printf("net: list rc=%d n=%d\n", rc, ln);
        CHECK(rc == 0 && ln > 0);
        for (int i = 0; i < ln && i < 6; i++) printf("  %s%s\n", l[i].name, l[i].dir ? "/" : "");
        char path[1024] = "";
        for (int i = 0; i < ln; i++) if (!l[i].dir) { rc = hvsc_fetch(l[i].name, path, sizeof path); printf("net: fetch %s rc=%d -> %s\n", l[i].name, rc, path); CHECK(rc == 0); break; }
        CHECK(hvsc_fetch("../x.sid", path, sizeof path) != 0);
        free(l);
    }
    printf(fails ? "hvsc: %d FAILED\n" : "hvsc: all ok\n", fails);
    return fails != 0;
}
