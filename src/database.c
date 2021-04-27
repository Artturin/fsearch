/*
   FSearch - A fast file search utility
   Copyright © 2020 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fnmatch.h>
#include <glib/gi18n.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "database.h"
#include "debug.h"
#include "fsearch_exclude_path.h"
#include "fsearch_include_path.h"
#include "memory_pool.h"
#include "utils.h"

#define BTREE_NODE_POOL_BLOCK_ELEMENTS 10000

struct _FsearchDatabase {
    DynamicArray *files;
    DynamicArray *folders;

    FsearchMemoryPool *file_pool;
    FsearchMemoryPool *folder_pool;

    uint32_t num_entries;
    uint32_t num_folders;
    uint32_t num_files;

    GList *locations;
    GList *includes;
    GList *excludes;
    char **exclude_files;
    DynamicArray *entries;

    bool exclude_hidden;
    time_t timestamp;

    int32_t ref_count;
    GMutex mutex;
};

struct _FsearchDatabaseNode {
    // B+ tree of entry nodes
    DatabaseEntry *entries;
    FsearchMemoryPool *pool;
    uint32_t num_items;
    uint32_t num_folders;
    uint32_t num_files;
};

typedef struct _FsearchDatabaseNode FsearchDatabaseNode;

enum {
    WALK_OK = 0,
    WALK_BADIO,
    WALK_CANCEL,
};

// Forward declarations
static void
db_entries_clear(FsearchDatabase *db);

static FsearchDatabaseNode *
db_location_get_for_path(FsearchDatabase *db, const char *path);

static FsearchDatabaseNode *
db_location_scan(FsearchDatabase *db, const char *dname, GCancellable *cancellable, void (*status_cb)(const char *));

static FsearchDatabaseNode *
db_location_new(void);

static void
db_location_free(FsearchDatabaseNode *location);

static void
db_list_add_location(FsearchDatabase *db, FsearchDatabaseNode *location);

// Implementation

static FsearchDatabaseEntryFile *
db_file_entry_new(const char *name, off_t size, FsearchDatabaseEntryFolder *parent) {
    FsearchDatabaseEntryFile *file_entry = calloc(1, sizeof(FsearchDatabaseEntryFile));
    assert(file_entry != NULL);

    if (name) {
        file_entry->name = strdup(name);
    }
    file_entry->size = size;
    file_entry->parent = parent;

    return file_entry;
}

static FsearchDatabaseEntryFolder *
db_folder_entry_new(const char *name, off_t size, FsearchDatabaseEntryFolder *parent) {
    FsearchDatabaseEntryFolder *folder_entry = calloc(1, sizeof(FsearchDatabaseEntryFolder));
    assert(folder_entry != NULL);

    if (name) {
        folder_entry->name = strdup(name);
    }
    folder_entry->size = size;
    folder_entry->parent = parent;
    folder_entry->file_children = g_ptr_array_new();
    folder_entry->folder_children = g_ptr_array_new();

    return folder_entry;
}

static void
db_sort(FsearchDatabase *db) {
    assert(db != NULL);
    assert(db->entries != NULL);

    trace("[database] sorting...\n");
    darray_sort(db->entries, (DynamicArrayCompareFunc)compare_name);
    trace("[database] sorted\n");
}

static void
db_update_timestamp(FsearchDatabase *db) {
    assert(db != NULL);
    db->timestamp = time(NULL);
}

static FsearchDatabaseNode *
db_location_load_from_file(const char *fname) {
    assert(fname != NULL);

    FILE *fp = fopen(fname, "rb");
    if (!fp) {
        return NULL;
    }

    DatabaseEntry *root = NULL;
    FsearchDatabaseNode *location = db_location_new();

    char magic[4];
    if (fread(magic, 4, 1, fp) != 1) {
        trace("[database_read_file] failed to read magic\n");
        goto load_fail;
    }
    if (strncmp(magic, "FSDB", 4)) {
        trace("[database_read_file] bad signature\n");
        goto load_fail;
    }

    uint8_t majorver = 0;
    if (fread(&majorver, 1, 1, fp) != 1) {
        goto load_fail;
    }
    if (majorver != 0) {
        trace("[database_read_file] bad majorver=%d\n", majorver);
        goto load_fail;
    }

    uint8_t minorver = 0;
    if (fread(&minorver, 1, 1, fp) != 1) {
        goto load_fail;
    }
    if (minorver != 1) {
        trace("[database_read_file] bad minorver=%d\n", minorver);
        goto load_fail;
    }
    trace("[database_read_file] database version=%d.%d\n", majorver, minorver);

    uint32_t num_items = 0;
    if (fread(&num_items, 4, 1, fp) != 1) {
        goto load_fail;
    }

    uint32_t num_folders = 0;
    uint32_t num_files = 0;

    uint32_t num_items_read = 0;
    DatabaseEntry *prev = NULL;
    while (true) {
        uint16_t name_len = 0;
        if (fread(&name_len, 2, 1, fp) != 1) {
            trace("[database_read_file] failed to read name length\n");
            goto load_fail;
        }

        if (name_len == 0) {
            // reached end of child marker
            if (!prev) {
                goto load_fail;
            }
            prev = prev->parent;
            if (!prev) {
                // prev was root node, we're done
                trace("[database_read_file] reached root node. done\n");
                break;
            }
            continue;
        }

        // read name
        char name[name_len + 1];
        if (fread(&name, name_len, 1, fp) != 1) {
            trace("[database_read_file] failed to read name\n");
            goto load_fail;
        }
        name[name_len] = '\0';

        // read is_dir
        uint8_t is_dir = 0;
        if (fread(&is_dir, 1, 1, fp) != 1) {
            trace("[database_read_file] failed to read is_dir\n");
            goto load_fail;
        }

        // read size
        uint64_t size = 0;
        if (fread(&size, 8, 1, fp) != 1) {
            trace("[database_read_file] failed to read size\n");
            goto load_fail;
        }

        // read mtime
        uint64_t mtime = 0;
        if (fread(&mtime, 8, 1, fp) != 1) {
            trace("[database_read_file] failed to read mtime\n");
            goto load_fail;
        }

        // read sort position
        uint32_t pos = 0;
        if (fread(&pos, 4, 1, fp) != 1) {
            trace("[database_read_file] failed to read sort position\n");
            goto load_fail;
        }

        int is_root = !strcmp(name, "/");
        DatabaseEntry *new = fsearch_memory_pool_malloc(location->pool);
        new->name = is_root ? strdup("/") : strdup(name);
        new->mtime = mtime;
        new->size = size;
        new->is_dir = is_dir;
        new->pos = pos;

        is_dir ? num_folders++ : num_files++;
        num_items_read++;

        if (!prev) {
            prev = new;
            root = new;
            continue;
        }
        prev = btree_node_prepend(prev, new);
    }
    trace("[database_load] finished with %d of %d items successfully read\n", num_items_read, num_items);

    location->num_items = num_items_read;
    location->num_folders = num_folders;
    location->num_files = num_files;
    location->entries = root;

    fclose(fp);

    return location;

load_fail:
    fprintf(stderr, "database load fail (%s)!\n", fname);
    if (fp) {
        fclose(fp);
    }
    db_location_free(location);
    return NULL;
}

static bool
db_location_write_to_file(FsearchDatabaseNode *location, const char *path) {
    assert(path != NULL);
    assert(location != NULL);

    if (!location->entries) {
        return false;
    }
    g_mkdir_with_parents(path, 0700);

    GString *db_path = g_string_new(path);
    g_string_append(db_path, "/database.db");

    FILE *fp = fopen(db_path->str, "w+b");
    if (!fp) {
        return false;
    }

    const char magic[] = "FSDB";
    if (fwrite(magic, 4, 1, fp) != 1) {
        goto save_fail;
    }

    const uint8_t majorver = 0;
    if (fwrite(&majorver, 1, 1, fp) != 1) {
        goto save_fail;
    }

    const uint8_t minorver = 1;
    if (fwrite(&minorver, 1, 1, fp) != 1) {
        goto save_fail;
    }

    uint32_t num_items = btree_node_n_nodes(location->entries);
    if (fwrite(&num_items, 4, 1, fp) != 1) {
        goto save_fail;
    }

    const uint16_t del = 0;

    DatabaseEntry *root = location->entries;
    DatabaseEntry *node = root;
    uint32_t is_root = !strcmp(root->name, "");

    while (node) {
        const char *name = is_root ? "/" : node->name;
        is_root = 0;
        uint16_t len = strlen(name);
        if (len) {
            // write length of node name
            if (fwrite(&len, 2, 1, fp) != 1) {
                goto save_fail;
            }
            // write node name
            if (fwrite(name, len, 1, fp) != 1) {
                goto save_fail;
            }
            // write is_dir
            uint8_t is_dir = node->is_dir;
            if (fwrite(&is_dir, 1, 1, fp) != 1) {
                goto save_fail;
            }

            // write node size
            uint64_t size = node->size;
            if (fwrite(&size, 8, 1, fp) != 1) {
                goto save_fail;
            }

            // write node modification time
            uint64_t mtime = node->mtime;
            if (fwrite(&mtime, 8, 1, fp) != 1) {
                goto save_fail;
            }

            // write node sort position
            uint32_t pos = node->pos;
            if (fwrite(&pos, 4, 1, fp) != 1) {
                goto save_fail;
            }

            DatabaseEntry *temp = node->children;
            if (!temp) {
                // reached end of children, write delimiter
                if (fwrite(&del, 2, 1, fp) != 1) {
                    goto save_fail;
                }
                DatabaseEntry *current = node;
                while (true) {
                    temp = current->next;
                    if (temp) {
                        // found next, sibling add that
                        node = temp;
                        break;
                    }

                    if (fwrite(&del, 2, 1, fp) != 1) {
                        goto save_fail;
                    }
                    temp = current->parent;
                    if (!temp) {
                        // reached last node, abort
                        node = NULL;
                        break;
                    }
                    else {
                        current = temp;
                    }
                }
            }
            else {
                node = temp;
            }
        }
        else {
            goto save_fail;
        }
    }

    fclose(fp);

    trace("[database_save] saved %s\n", path);
    return true;

save_fail:

    fclose(fp);
    unlink(db_path->str);
    g_string_free(db_path, TRUE);
    return false;
}

static bool
file_is_excluded(const char *name, char **exclude_files) {
    if (exclude_files) {
        for (int i = 0; exclude_files[i]; ++i) {
            if (!fnmatch(exclude_files[i], name, 0)) {
                return true;
            }
        }
    }
    return false;
}

static bool
directory_is_excluded(const char *name, GList *excludes) {
    while (excludes) {
        FsearchExcludePath *fs_path = excludes->data;
        if (!strcmp(name, fs_path->path)) {
            if (fs_path->enabled) {
                return true;
            }
            return false;
        }
        excludes = excludes->next;
    }
    return false;
}

typedef struct DatabaseWalkContext {
    FsearchDatabase *db;
    FsearchDatabaseNode *db_node;
    GString *path;
    GTimer *timer;
    GCancellable *cancellable;
    void (*status_cb)(const char *);
    bool exclude_hidden;
} DatabaseWalkContext;

static int
db_location_walk_tree_recursive(DatabaseWalkContext *walk_context, DatabaseEntry *parent) {

    if (walk_context->cancellable && g_cancellable_is_cancelled(walk_context->cancellable)) {
        return WALK_CANCEL;
    }

    GString *path = walk_context->path;
    g_string_append_c(path, '/');

    // remember end of parent path
    gsize path_len = path->len;

    DIR *dir = NULL;
    if (!(dir = opendir(path->str))) {
        return WALK_BADIO;
    }

    double elapsed_seconds = g_timer_elapsed(walk_context->timer, NULL);
    if (elapsed_seconds > 0.1) {
        if (walk_context->status_cb) {
            walk_context->status_cb(path->str);
        }
        g_timer_start(walk_context->timer);
    }

    struct dirent *dent = NULL;
    while ((dent = readdir(dir))) {
        if (walk_context->cancellable && g_cancellable_is_cancelled(walk_context->cancellable)) {
            closedir(dir);
            return WALK_CANCEL;
        }
        if (walk_context->exclude_hidden && dent->d_name[0] == '.') {
            // file is dotfile, skip
            continue;
        }
        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) {
            continue;
        }
        if (file_is_excluded(dent->d_name, walk_context->db->exclude_files)) {
            continue;
        }

        // create full path of file/folder
        g_string_truncate(path, path_len);
        g_string_append(path, dent->d_name);

        struct stat st;
        if (lstat(path->str, &st) == -1) {
            // warn("Can't stat %s", fn);
            continue;
        }

        const bool is_dir = S_ISDIR(st.st_mode);
        if (is_dir && directory_is_excluded(path->str, walk_context->db->excludes)) {
            trace("[database_scan] excluded directory: %s\n", path->str);
            continue;
        }

        DatabaseEntry *node = fsearch_memory_pool_malloc(walk_context->db_node->pool);
        node->name = strdup(dent->d_name);
        node->mtime = st.st_mtime;
        node->size = st.st_size;
        node->is_dir = is_dir;
        node->pos = 0;

        btree_node_prepend(parent, node);
        walk_context->db_node->num_items++;
        if (is_dir) {
            walk_context->db_node->num_folders++;
            db_location_walk_tree_recursive(walk_context, node);
        }
        else {
            walk_context->db_node->num_files++;
        }
    }

    if (dir) {
        closedir(dir);
    }
    return WALK_OK;
}

static void
db_location_free(FsearchDatabaseNode *location) {
    assert(location != NULL);

    if (location->pool) {
        fsearch_memory_pool_free(location->pool);
        location->pool = NULL;
    }
    g_free(location);
    location = NULL;
}

static int
db_folder_scan_recursive(DatabaseWalkContext *walk_context, FsearchDatabaseEntryFolder *parent) {
    if (walk_context->cancellable && g_cancellable_is_cancelled(walk_context->cancellable)) {
        return WALK_CANCEL;
    }

    GString *path = walk_context->path;
    g_string_append_c(path, '/');

    // remember end of parent path
    gsize path_len = path->len;

    DIR *dir = NULL;
    if (!(dir = opendir(path->str))) {
        return WALK_BADIO;
    }

    double elapsed_seconds = g_timer_elapsed(walk_context->timer, NULL);
    if (elapsed_seconds > 0.1) {
        if (walk_context->status_cb) {
            walk_context->status_cb(path->str);
        }
        g_timer_start(walk_context->timer);
    }

    FsearchDatabase *db = walk_context->db;

    struct dirent *dent = NULL;
    while ((dent = readdir(dir))) {
        if (walk_context->cancellable && g_cancellable_is_cancelled(walk_context->cancellable)) {
            closedir(dir);
            return WALK_CANCEL;
        }
        if (walk_context->exclude_hidden && dent->d_name[0] == '.') {
            // file is dotfile, skip
            continue;
        }
        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) {
            continue;
        }
        if (file_is_excluded(dent->d_name, db->exclude_files)) {
            continue;
        }

        // create full path of file/folder
        g_string_truncate(path, path_len);
        g_string_append(path, dent->d_name);

        struct stat st;
        if (lstat(path->str, &st) == -1) {
            // warn("Can't stat %s", fn);
            continue;
        }

        const bool is_dir = S_ISDIR(st.st_mode);
        if (is_dir && directory_is_excluded(path->str, db->excludes)) {
            trace("[database_scan] excluded directory: %s\n", path->str);
            continue;
        }

        if (is_dir) {
            FsearchDatabaseEntryFolder *folder_entry = fsearch_memory_pool_malloc(db->folder_pool);
            folder_entry->name = strdup(dent->d_name);
            folder_entry->parent = parent;

            parent->size += folder_entry->size;

            if (!parent->folder_children) {
                parent->folder_children = g_ptr_array_new();
            }
            g_ptr_array_add(parent->folder_children, folder_entry);
            darray_add_item(db->folders, folder_entry);

            db->num_folders++;

            db_folder_scan_recursive(walk_context, folder_entry);
        }
        else {
            FsearchDatabaseEntryFile *file_entry = fsearch_memory_pool_malloc(db->file_pool);
            file_entry->name = strdup(dent->d_name);
            file_entry->size = st.st_size;
            file_entry->parent = parent;

            parent->size += file_entry->size;

            if (!parent->file_children) {
                parent->file_children = g_ptr_array_new();
            }
            g_ptr_array_add(parent->file_children, file_entry);
            darray_add_item(db->files, file_entry);

            db->num_files++;
        }

        db->num_entries++;
    }

    if (dir) {
        closedir(dir);
    }
    return WALK_OK;
}

static void
db_folder_scan(FsearchDatabase *db, const char *dname, GCancellable *cancellable, void (*status_cb)(const char *)) {
    printf("folder scan\n");

    GTimer *timer = g_timer_new();
    GString *path = g_string_new(dname);
    printf("scan path: %s\n", path->str);

    g_timer_start(timer);
    DatabaseWalkContext walk_context = {
        .db = db,
        .path = path,
        .timer = timer,
        .cancellable = cancellable,
        .status_cb = status_cb,
        .exclude_hidden = db->exclude_hidden,
    };

    FsearchDatabaseEntryFolder *parent = fsearch_memory_pool_malloc(db->folder_pool);
    parent->name = strdup(dname);
    parent->parent = NULL;
    uint32_t res = db_folder_scan_recursive(&walk_context, parent);

    g_string_free(path, TRUE);
    g_timer_destroy(timer);
    if (res == WALK_OK) {
        printf("scanned: %d,%d,%d\n", db->num_files, db->num_folders, db->num_entries);
        return;
    }

    trace("[database_scan] walk error: %d\n", res);
}

static FsearchDatabaseNode *
db_location_scan(FsearchDatabase *db, const char *dname, GCancellable *cancellable, void (*status_cb)(const char *)) {
    const char *root_name = NULL;
    if (!strcmp(dname, "/")) {
        root_name = "";
    }
    else {
        root_name = dname;
    }
    FsearchDatabaseNode *location = db_location_new();
    DatabaseEntry *root = fsearch_memory_pool_malloc(location->pool);
    root->name = strdup(root_name);
    root->mtime = 0;
    root->size = 0;
    root->is_dir = true;
    root->pos = 0;

    location->entries = root;

    GTimer *timer = g_timer_new();
    GString *path = NULL;
    if (!strcmp(dname, "/")) {
        path = g_string_new(NULL);
    }
    else {
        path = g_string_new(dname);
    }

    g_timer_start(timer);
    DatabaseWalkContext walk_context = {
        .db = db,
        .db_node = location,
        .path = path,
        .timer = timer,
        .cancellable = cancellable,
        .status_cb = status_cb,
        .exclude_hidden = db->exclude_hidden,
    };

    uint32_t res = db_location_walk_tree_recursive(&walk_context, root);

    g_string_free(path, TRUE);
    g_timer_destroy(timer);
    if (res == WALK_OK) {
        return location;
    }

    trace("[database_scan] walk error: %d\n", res);
    db_location_free(location);
    return NULL;
}

static FsearchDatabaseNode *
db_location_new(void) {
    FsearchDatabaseNode *location = g_new0(FsearchDatabaseNode, 1);
    location->pool = fsearch_memory_pool_new(BTREE_NODE_POOL_BLOCK_ELEMENTS,
                                             sizeof(DatabaseEntry),
                                             (GDestroyNotify)btree_node_clear);
    return location;
}

static bool
db_list_insert_node(DatabaseEntry *node, void *data) {
    FsearchDatabase *db = data;
    darray_set_item(db->entries, node, node->pos);
    node->is_dir ? db->num_folders++ : db->num_files++;
    db->num_entries++;
    return true;
}

static void
db_traverse_tree_insert(DatabaseEntry *node, void *data) {
    btree_node_traverse(node, db_list_insert_node, data);
}

static bool
db_list_add_node(DatabaseEntry *node, void *data) {
    FsearchDatabase *db = data;
    darray_add_item(db->entries, node);
    node->is_dir ? db->num_folders++ : db->num_files++;
    db->num_entries++;
    return true;
}

static void
db_traverse_tree_add(DatabaseEntry *node, void *data) {
    btree_node_traverse(node, db_list_add_node, data);
}

static void
db_list_insert_location(FsearchDatabase *db, FsearchDatabaseNode *location) {
    assert(db != NULL);
    assert(location != NULL);
    assert(location->entries != NULL);

    btree_node_children_foreach(location->entries, db_traverse_tree_insert, db);
}

static void
db_list_add_location(FsearchDatabase *db, FsearchDatabaseNode *location) {
    assert(db != NULL);
    assert(location != NULL);
    assert(location->entries != NULL);

    btree_node_children_foreach(location->entries, db_traverse_tree_add, db);
}

static FsearchDatabaseNode *
db_location_get_for_path(FsearchDatabase *db, const char *path) {
    assert(db != NULL);
    assert(path != NULL);

    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        FsearchDatabaseNode *location = (FsearchDatabaseNode *)l->data;
        DatabaseEntry *root = btree_node_get_root(location->entries);
        const char *location_path = root->name;
        if (!strcmp(location_path, path)) {
            return location;
        }
    }
    return NULL;
}

// static bool
// db_location_remove(FsearchDatabase *db, const char *path) {
//    assert(db != NULL);
//    assert(path != NULL);
//
//    FsearchDatabaseNode *location = db_location_get_for_path(db, path);
//    if (location) {
//        db->locations = g_list_remove(db->locations, location);
//        db_location_free(location);
//        db_sort(db);
//    }
//
//    return true;
//}

static void
location_build_path(char *path, size_t path_len, const char *location_name) {
    assert(path != NULL);
    assert(location_name != NULL);

    const char *location = !strcmp(location_name, "") ? "/" : location_name;

    gchar *path_checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, location, -1);

    assert(path_checksum != NULL);

    gchar data_dir[PATH_MAX] = "";
    init_data_dir_path(data_dir, sizeof(data_dir));

    assert(0 <= snprintf(path, path_len, "%s/database/%s", data_dir, path_checksum));
    g_free(path_checksum);
    return;
}

// static void
// db_location_delete(FsearchDatabaseNode *location, const char *location_name) {
//    assert(location != NULL);
//    assert(location_name != NULL);
//
//    gchar database_path[PATH_MAX] = "";
//    location_build_path(database_path, sizeof(database_path), location_name);
//
//    gchar database_file_path[PATH_MAX] = "";
//    assert(0 <= snprintf(database_file_path, sizeof(database_file_path), "%s/%s", database_path, "database.db"));
//
//    g_remove(database_file_path);
//    g_remove(database_path);
//}

static bool
db_save_location(FsearchDatabase *db, const char *location_name) {
    assert(db != NULL);

    gchar database_path[PATH_MAX] = "";
    location_build_path(database_path, sizeof(database_path), location_name);
    trace("[database_save] saving %s to %s\n", location_name, database_path);

    gchar database_fname[PATH_MAX] = "";
    assert(0 <= snprintf(database_fname, sizeof(database_fname), "%s/database.db", database_path));
    FsearchDatabaseNode *location = db_location_get_for_path(db, location_name);
    if (location) {
        db_location_write_to_file(location, database_path);
    }

    return true;
}

bool
db_save(FsearchDatabase *db) {
    assert(db != NULL);
    return false;

    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        FsearchDatabaseNode *location = (FsearchDatabaseNode *)l->data;
        DatabaseEntry *root = btree_node_get_root(location->entries);
        const char *location_path = root->name;
        db_save_location(db, location_path);
    }
    return true;
}

static gchar *
db_location_get_path(const char *location_name) {
    gchar database_path[PATH_MAX] = "";
    location_build_path(database_path, sizeof(database_path), location_name);

    gchar database_fname[PATH_MAX] = "";
    assert(0 <= snprintf(database_fname, sizeof(database_fname), "%s/database.db", database_path));

    return g_strdup(database_fname);
}

static FsearchDatabaseNode *
db_location_load(FsearchDatabase *db, const char *location_name) {
    return NULL;
    gchar *load_path = db_location_get_path(location_name);
    if (!load_path) {
        return NULL;
    }
    FsearchDatabaseNode *location = db_location_load_from_file(load_path);
    g_free(load_path);
    load_path = NULL;

    return location;
}

static void
db_location_add(FsearchDatabase *db, FsearchDatabaseNode *location) {
    assert(db != NULL);
    assert(location != NULL);

    db->locations = g_list_append(db->locations, location);
    db->num_entries += location->num_items;
    db->num_folders += location->num_folders;
    db->num_files += location->num_files;
    db_update_timestamp(db);
}

static void
db_update_sort_index(FsearchDatabase *db) {
    assert(db != NULL);
    assert(db->entries != NULL);

    for (uint32_t i = 0; i < db->num_entries; ++i) {
        DatabaseEntry *node = darray_get_item(db->entries, i);
        node->pos = i;
    }
}

static uint32_t
db_locations_get_num_entries(FsearchDatabase *db) {
    assert(db != NULL);
    assert(db->locations != NULL);

    uint32_t num_entries = 0;
    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        FsearchDatabaseNode *location = l->data;
        num_entries += location->num_items;
    }
    return num_entries;
}

static void
db_list_build(FsearchDatabase *db, void (*status_cb)(const char *)) {
    assert(db != NULL);
    assert(db->num_entries >= 0);

    db_entries_clear(db);
    uint32_t num_entries = db_locations_get_num_entries(db);
    trace("[database_build_list] create list for %d entries\n", num_entries);
    db->entries = darray_new(num_entries);

    if (status_cb) {
        status_cb(_("Building lookup list…"));
    }
    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        db_list_add_location(db, l->data);
    }
    if (status_cb) {
        status_cb(_("Sorting…"));
    }
    db_sort(db);
    db_update_sort_index(db);
    trace("[database_build_list] list created\n");
}

static void
db_list_update(FsearchDatabase *db) {
    assert(db != NULL);
    assert(db->num_entries >= 0);

    db_entries_clear(db);
    uint32_t num_entries = db_locations_get_num_entries(db);
    trace("[database_update_list] create list for %d entries\n", num_entries);
    db->entries = darray_new(num_entries);

    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        db_list_insert_location(db, l->data);
    }
    trace("[database_update_list] updated list\n");
}

// static DatabaseEntry *
// db_location_get_entries(FsearchDatabaseNode *location) {
//    assert(location != NULL);
//    return location->entries;
//}

FsearchDatabase *
db_new(GList *includes, GList *excludes, char **exclude_files, bool exclude_hidden) {
    FsearchDatabase *db = g_new0(FsearchDatabase, 1);
    g_mutex_init(&db->mutex);
    if (includes) {
        db->includes = g_list_copy_deep(includes, (GCopyFunc)fsearch_include_path_copy, NULL);
    }
    if (excludes) {
        db->excludes = g_list_copy_deep(excludes, (GCopyFunc)fsearch_exclude_path_copy, NULL);
    }
    if (exclude_files) {
        db->exclude_files = g_strdupv(exclude_files);
    }
    db->file_pool = fsearch_memory_pool_new(BTREE_NODE_POOL_BLOCK_ELEMENTS, sizeof(FsearchDatabaseEntryFile), NULL);
    db->folder_pool = fsearch_memory_pool_new(BTREE_NODE_POOL_BLOCK_ELEMENTS, sizeof(FsearchDatabaseEntryFolder), NULL);
    db->files = darray_new(1000);
    db->folders = darray_new(1000);
    db->exclude_hidden = exclude_hidden;
    db->ref_count = 1;
    return db;
}

static void
db_entries_clear(FsearchDatabase *db) {
    // free entries
    assert(db != NULL);

    if (db->entries) {
        darray_free(db->entries);
        db->entries = NULL;
    }
    db->num_entries = 0;
    db->num_folders = 0;
    db->num_files = 0;
}

static void
db_location_free_all(FsearchDatabase *db) {
    assert(db != NULL);
    if (!db->locations) {
        return;
    }

    trace("[database_location_free_all] freeing...\n");
    GList *l = db->locations;
    while (l) {
        db_location_free(l->data);
        l = l->next;
    }
    g_list_free(db->locations);
    db->locations = NULL;
    trace("[database_location_free_all] freed\n");
}

static void
db_free(FsearchDatabase *db) {
    assert(db != NULL);

    trace("[database_free] freeing...\n");
    db_lock(db);
    if (db->ref_count > 0) {
        trace("[database_free] pending references on free: %d\n", db->ref_count);
    }
    db_entries_clear(db);
    db_location_free_all(db);

    if (db->files) {
        darray_free(db->files);
        db->files = NULL;
    }
    if (db->folders) {
        darray_free(db->folders);
        db->folders = NULL;
    }
    if (db->folder_pool) {
        fsearch_memory_pool_free(db->folder_pool);
        db->folder_pool = NULL;
    }
    if (db->file_pool) {
        fsearch_memory_pool_free(db->file_pool);
        db->file_pool = NULL;
    }
    if (db->includes) {
        g_list_free_full(db->includes, (GDestroyNotify)fsearch_include_path_free);
        db->includes = NULL;
    }
    if (db->excludes) {
        g_list_free_full(db->excludes, (GDestroyNotify)fsearch_exclude_path_free);
        db->excludes = NULL;
    }
    if (db->exclude_files) {
        g_strfreev(db->exclude_files);
        db->exclude_files = NULL;
    }
    db_unlock(db);

    g_mutex_clear(&db->mutex);
    g_free(db);
    db = NULL;

    malloc_trim(0);

    trace("[database_free] freed\n");
    return;
}

time_t
db_get_timestamp(FsearchDatabase *db) {
    assert(db != NULL);
    return db->timestamp;
}

uint32_t
db_get_num_files(FsearchDatabase *db) {
    assert(db != NULL);
    return db->num_files;
}

uint32_t
db_get_num_folders(FsearchDatabase *db) {
    assert(db != NULL);
    return db->num_folders;
}

uint32_t
db_get_num_entries(FsearchDatabase *db) {
    assert(db != NULL);
    return db->num_entries;
}

void
db_unlock(FsearchDatabase *db) {
    assert(db != NULL);
    g_mutex_unlock(&db->mutex);
}

void
db_lock(FsearchDatabase *db) {
    assert(db != NULL);
    g_mutex_lock(&db->mutex);
}

bool
db_try_lock(FsearchDatabase *db) {
    assert(db != NULL);
    return g_mutex_trylock(&db->mutex);
}

DynamicArray *
db_get_files(FsearchDatabase *db) {
    assert(db != NULL);
    return db->files;
}

DynamicArray *
db_get_folders(FsearchDatabase *db) {
    assert(db != NULL);
    return db->folders;
}

DynamicArray *
db_get_entries(FsearchDatabase *db) {
    assert(db != NULL);
    return db->entries;
}

bool
db_load(FsearchDatabase *db, const char *path, void (*status_cb)(const char *)) {
    assert(db != NULL);
    return false;

    bool ret = false;
    for (GList *l = db->includes; l != NULL; l = l->next) {
        FsearchIncludePath *fs_path = l->data;
        if (!fs_path->enabled) {
            continue;
        }
        FsearchDatabaseNode *location = db_location_load(db, fs_path->path);
        if (location) {
            db_location_add(db, location);
            ret = true;
        }
    }
    if (ret) {
        db_list_update(db);
    }
    return ret;
}

int
sort_file_by_name(FsearchDatabaseEntryFile **a, FsearchDatabaseEntryFile **b) {
    return strverscmp((*a)->name, (*b)->name);
}

int
sort_folder_by_name(FsearchDatabaseEntryFolder **a, FsearchDatabaseEntryFolder **b) {
    return strverscmp((*a)->name, (*b)->name);
}

bool
db_scan(FsearchDatabase *db, GCancellable *cancellable, void (*status_cb)(const char *)) {
    assert(db != NULL);

    bool ret = false;
    bool init_list = false;
    for (GList *l = db->includes; l != NULL; l = l->next) {
        FsearchIncludePath *fs_path = l->data;
        if (!fs_path->path) {
            continue;
        }
        if (!fs_path->enabled) {
            continue;
        }
        FsearchDatabaseNode *location = NULL;
        if (fs_path->update) {
            // location = db_location_scan(db, fs_path->path, cancellable, status_cb);
            db_folder_scan(db, fs_path->path, cancellable, status_cb);
        }

        if (location != NULL) {
            init_list = true;
        }
        else {
            location = db_location_load(db, fs_path->path);
        }

        if (location != NULL) {
            db_location_add(db, location);
            ret = true;
        }
    }
    GTimer *timer = g_timer_new();
    printf("sorting\n");
    darray_sort_multi_threaded(db->files, (DynamicArrayCompareFunc)sort_file_by_name);
    darray_sort_multi_threaded(db->folders, (DynamicArrayCompareFunc)sort_folder_by_name);
    double seconds = g_timer_elapsed(timer, NULL);
    printf("sorted: %f\n", seconds);
    g_timer_destroy(timer);
    ////darray_sort(db->entries, (DynamicArrayCompareFunc)compare_name);
    // if (ret) {
    //     if (init_list) {
    //         db_list_build(db, status_cb);
    //     }
    //     else {
    //         db_list_update(db);
    //     }
    // }
    return ret;
}

void
db_ref(FsearchDatabase *db) {
    assert(db != NULL);
    db_lock(db);
    db->ref_count++;
    db_unlock(db);
    // trace("[database_ref] increased to: %d\n", db->ref_count);
}

void
db_unref(FsearchDatabase *db) {
    assert(db != NULL);
    db_lock(db);
    db->ref_count--;
    db_unlock(db);
    // trace("[database_unref] dropped to: %d\n", db->ref_count);
    if (db->ref_count <= 0) {
        db_free(db);
    }
}
