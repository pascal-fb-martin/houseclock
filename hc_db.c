/* houseclock - A simple GPS Time Server with Web console
 *
 * Copyright 2019, Pascal Martin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *
 * hc_db.c - The module that manage the database shared between
 *           the time synchronization module and the HTTP server.
 *
 * This is a simplified object store, where each object (table) is an array
 * of one or more records.
 *
 * SYNOPSYS:
 *
 * int hc_db_create (int size);
 *
 *    Create the live database as empty. This should be called once,
 *    in the main process before any fork() or clone(). A child process
 *    must not call this.
 *    Any pre-existing database is wiped out. Return 0 on success,
 *    errno value otherwise.
 *
 * int hc_db_new (const char *name, int size, int count);
 *
 *    Create a new table (array) with the specified size. Return 0
 *    on success, errno value otherwise.
 *
 * int hc_db_get_size (const char *name);
 * int hc_db_get_count (const char *name);
 *
 *    Get the record's size or the count of records for the specified table.
 *
 * void *hc_db_get (const char *name);
 *
 *    Access the data for the specified table.
 *
 * int hc_db_get_space (void);
 * int hc_db_get_used  (void);
 *
 *    Get information about shared memory usage.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "houseclock.h"
#include "hc_db.h"

#define HC_DB_DEFAULTSIZE  (1024*1024)

typedef struct {
    int next;
    int size;
    char name[32];
} hc_db_link;

#define HC_DB_MODULO 61

typedef struct {
    int size;
    int used;
    int index[HC_DB_MODULO];
} hc_db_head;

typedef struct {
    hc_db_link link;
    int count;
    int record;
} hc_db_table;

static hc_db_head *hc_db = 0;

/* This hash function is derived from Daniel J. Bernstein's djb2 hash function.
 */
static int hc_db_hash (const char *name, int modulo) {

    unsigned int hash = 5381;
    int c;
    while ((c = *name++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return (int) (hash % modulo);
}

int hc_db_create (int size) {
    if (size <= 0) {
        size = HC_DB_DEFAULTSIZE;
    }
    hc_db = (hc_db_head *)
        mmap (NULL, (size_t)size, PROT_READ+PROT_WRITE,
              MAP_SHARED|MAP_ANONYMOUS, -1, (off_t) 0);
    if (hc_db == MAP_FAILED) {
        fprintf (stderr, "cannot map shared memory (%d bytes): %s\n",
                 size, strerror(errno));
    }
    hc_db->size = size;
    hc_db->used = sizeof(hc_db_head);
    memset (hc_db->index, 0, HC_DB_MODULO * sizeof(int));
    return 0;
}

static hc_db_table *hc_db_search (const char *name) {
    int hash = hc_db_hash(name, HC_DB_MODULO);
    int offset = hc_db->index[hash];
    while (offset) {
        hc_db_table *table = (hc_db_table *)(((char *)hc_db) + offset);
        if (strcmp(table->link.name, name) == 0) return table;
        offset = table->link.next;
    }
    return 0;
}

int hc_db_new (const char *name, int size, int count) {
    hc_db_table *table = hc_db_search (name);
    if (table) return EEXIST;
    if (size <= 0 || count <= 0) return EINVAL;
    int total = sizeof(hc_db_head) + (size * count);
    if (total > hc_db->size - hc_db->used) return ENOMEM;

    table = (hc_db_table *)(((char *)hc_db) + hc_db->used);
    int hash = hc_db_hash(name, HC_DB_MODULO);
    table->link.size = size * count;
    table->link.next = hc_db->index[hash];
    strncpy (table->link.name, name, sizeof(table->link.name));
    table->link.name[sizeof(table->link.name)-1] = 0;
    table->count = count;
    table->record = size;
    hc_db->index[hash] = hc_db->used;
    hc_db->used += total;
    return 0;
}   

int hc_db_get_size (const char *name) {
    hc_db_table *table = hc_db_search(name);
    if (table) return table->record;
    return 0;
}

int hc_db_get_count (const char *name) {
    hc_db_table *table = hc_db_search(name);
    if (table) return table->count;
    return 0;
}

void *hc_db_get (const char *name) {
    hc_db_table *table = hc_db_search(name);
    if (table) return (void *)(table+1);
    return 0;
}

int hc_db_get_space (void) {
    return hc_db->size;
}

int hc_db_get_used  (void) {
    return hc_db->used;
}

