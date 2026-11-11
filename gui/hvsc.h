/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Online browsing of the High Voltage SID Collection (HVSC).
 *
 * Folder listings and single .sid files are fetched on demand from an HTTP mirror by running
 * the system's `curl` as a child process (no extra library). Nothing but the tunes you play is
 * stored: they are cached under $XDG_CACHE_HOME/ngsid (or ~/.cache/ngsid).
 *
 * Paths are relative to the collection root, decoded, with '/' separators and no leading
 * slash; a folder ends with '/', e.g. "MUSICIANS/H/Hubbard_Rob/". The root is "".
 * Set NGSID_HVSC_MIRROR to a base URL (ending in /) to use another mirror. */
#ifndef HVSC_H
#define HVSC_H

#include <stddef.h>

#define HVSC_ERR_NOCURL (-2)

typedef struct {
    char name[160];   /* decoded; folders without the trailing '/' */
    int dir;
} hvsc_entry;

/* Parse an Apache-style index page. Keeps sub folders and .sid files, folders first, sorted. */
int hvsc_parse_listing(const char *html, size_t len, hvsc_entry **out, int *n);

/* Blocking calls (run them in a worker thread). 0 = ok, HVSC_ERR_NOCURL = curl missing, else -1. */
int hvsc_list(const char *folder, hvsc_entry **out, int *n);
int hvsc_fetch(const char *file, char *cache_path, size_t cap);

/* Web build only: resolve a HVSC database id (hvsc.c64.org) to its path. 0 = ok, else -1. */
int hvsc_resolve_id(int id, char *path, size_t cap);

/* Blocking HTTPS GET through curl into a malloc'd buffer (caller frees). Same return codes. */
int hvsc_http_get(const char *url, char **buf, size_t *len);

#endif
