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

#define G_LOG_DOMAIN "fsearch-database"

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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "fsearch_database.h"
#include "fsearch_exclude_path.h"
#include "fsearch_include_path.h"
#include "fsearch_memory_pool.h"
#include "fsearch_utils.h"

#define BTREE_NODE_POOL_BLOCK_ELEMENTS 10000

struct FsearchDatabaseEntryCommon {
    FsearchDatabaseEntryFolder *parent;
    char *name;
    off_t size;

    uint8_t type;
};

struct _FsearchDatabaseEntryFile {
    struct FsearchDatabaseEntryCommon shared;
};

struct _FsearchDatabaseEntryFolder {
    struct FsearchDatabaseEntryCommon shared;

    GSList *folder_children;
    GSList *file_children;

    uint32_t idx;
};

struct _FsearchDatabase {
    DynamicArray *files;
    DynamicArray *folders;

    FsearchMemoryPool *file_pool;
    FsearchMemoryPool *folder_pool;

    uint32_t num_entries;
    uint32_t num_folders;
    uint32_t num_files;

    GList *includes;
    GList *excludes;
    char **exclude_files;

    bool exclude_hidden;
    time_t timestamp;

    int32_t ref_count;
    GMutex mutex;
};

enum {
    WALK_OK = 0,
    WALK_BADIO,
    WALK_CANCEL,
};

// Implementation

static void
db_file_entry_destroy(FsearchDatabaseEntryFolder *entry) {
    if (!entry) {
        return;
    }
    if (entry->shared.name) {
        free(entry->shared.name);
        entry->shared.name = NULL;
    }
}

static void
db_folder_entry_destroy(FsearchDatabaseEntryFolder *entry) {
    if (!entry) {
        return;
    }
    if (entry->shared.name) {
        free(entry->shared.name);
        entry->shared.name = NULL;
    }
    if (entry->file_children) {
        g_slist_free(entry->file_children);
        entry->file_children = NULL;
    }
    if (entry->folder_children) {
        g_slist_free(entry->folder_children);
        entry->folder_children = NULL;
    }
}

static uint32_t
db_entry_get_depth(FsearchDatabaseEntry *entry) {
    uint32_t depth = 0;
    while (entry && entry->shared.parent) {
        entry = (FsearchDatabaseEntry *)entry->shared.parent;
        depth++;
    }
    return depth;
}

static FsearchDatabaseEntryFolder *
db_entry_get_parent_nth(FsearchDatabaseEntryFolder *entry, int32_t nth) {
    while (entry && nth > 0) {
        entry = entry->shared.parent;
        nth--;
    }
    return entry;
}

static void
sort_entry_by_path_recursive(FsearchDatabaseEntryFolder *entry_a, FsearchDatabaseEntryFolder *entry_b, int *res) {
    if (!entry_a) {
        return;
    }
    if (entry_a->shared.parent && entry_a->shared.parent != entry_b->shared.parent) {
        sort_entry_by_path_recursive(entry_a->shared.parent, entry_b->shared.parent, res);
    }
    if (*res != 0) {
        return;
    }
    *res = strverscmp(entry_a->shared.name, entry_b->shared.name);
}

int
db_entry_compare_entries_by_size(FsearchDatabaseEntry **a, FsearchDatabaseEntry **b) {
    off_t size_a = db_entry_get_size(*a);
    off_t size_b = db_entry_get_size(*b);
    return (size_a > size_b) ? 1 : -1;
}

int
db_entry_compare_entries_by_type(FsearchDatabaseEntry **a, FsearchDatabaseEntry **b) {
    FsearchDatabaseEntryType type_a = db_entry_get_type(*a);
    FsearchDatabaseEntryType type_b = db_entry_get_type(*b);
    if (type_a == DATABASE_ENTRY_TYPE_FOLDER && type_b == DATABASE_ENTRY_TYPE_FOLDER) {
        return 0;
    }

    const char *name_a = db_entry_get_name(*a);
    const char *name_b = db_entry_get_name(*b);
    char *file_type_a = get_file_type_non_localized(name_a, FALSE);
    char *file_type_b = get_file_type_non_localized(name_b, FALSE);

    int return_val = strcmp(file_type_a, file_type_b);
    g_free(file_type_a);
    file_type_a = NULL;
    g_free(file_type_b);
    file_type_b = NULL;

    return return_val;
}

int
db_entry_compare_entries_by_modification_time(FsearchDatabaseEntry **a, FsearchDatabaseEntry **b) {
    return 0;
}

int
db_entry_compare_entries_by_position(FsearchDatabaseEntry **a, FsearchDatabaseEntry **b) {
    return 0;
}

int
db_entry_compare_entries_by_path(FsearchDatabaseEntry **a, FsearchDatabaseEntry **b) {
    FsearchDatabaseEntry *entry_a = *a;
    FsearchDatabaseEntry *entry_b = *b;
    uint32_t a_depth = db_entry_get_depth(entry_a);
    uint32_t b_depth = db_entry_get_depth(entry_b);

    int res = 0;
    if (a_depth == b_depth) {
        sort_entry_by_path_recursive(entry_a->shared.parent, entry_b->shared.parent, &res);
    }
    else if (a_depth > b_depth) {
        int32_t diff = a_depth - b_depth;
        FsearchDatabaseEntryFolder *parent_a = db_entry_get_parent_nth(entry_a->shared.parent, diff);
        sort_entry_by_path_recursive(parent_a, entry_b->shared.parent, &res);
        res = res == 0 ? 1 : res;
    }
    else {
        int32_t diff = b_depth - a_depth;
        FsearchDatabaseEntryFolder *parent_b = db_entry_get_parent_nth(entry_b->shared.parent, diff);
        sort_entry_by_path_recursive(entry_a->shared.parent, parent_b, &res);
        res = res == 0 ? -1 : res;
    }
    return res;
}

int
db_entry_compare_entries_by_name(FsearchDatabaseEntry **a, FsearchDatabaseEntry **b) {
    return strverscmp((*a)->shared.name, (*b)->shared.name);
}

static void
db_sort(FsearchDatabase *db) {
    assert(db != NULL);

    GTimer *timer = g_timer_new();
    if (db->files) {
        darray_sort_multi_threaded(db->files, (DynamicArrayCompareFunc)db_entry_compare_entries_by_path);
        darray_sort(db->files, (DynamicArrayCompareFunc)db_entry_compare_entries_by_name);
        const double seconds = g_timer_elapsed(timer, NULL);
        g_timer_reset(timer);
        g_debug("[database] sorted files: %f s", seconds);
    }
    if (db->folders) {
        darray_sort_multi_threaded(db->folders, (DynamicArrayCompareFunc)db_entry_compare_entries_by_path);
        darray_sort(db->folders, (DynamicArrayCompareFunc)db_entry_compare_entries_by_name);
        const double seconds = g_timer_elapsed(timer, NULL);
        g_debug("[database] sorted folders: %f s", seconds);
    }
    g_timer_destroy(timer);
    timer = NULL;
}

static void
db_update_timestamp(FsearchDatabase *db) {
    assert(db != NULL);
    db->timestamp = time(NULL);
}

static void
db_entry_update_folder_indices(FsearchDatabase *db) {
    if (!db || !db->folders) {
        return;
    }
    uint32_t num_folders = darray_get_num_items(db->folders);
    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(db->folders, i);
        if (!folder) {
            continue;
        }
        folder->idx = i;
    }
}

static uint8_t
get_name_offset(const char *old, const char *new) {
    uint8_t offset = 0;
    while (old[offset] == new[offset] && old[offset] != '\0' && new[offset] != '\0') {
        offset++;
    }
    return offset;
}

static FILE *
db_file_open_locked(const char *file_path, const char *mode) {
    FILE *file_pointer = fopen(file_path, mode);
    if (!file_pointer) {
        g_debug("can't open database file: %s", file_path);
        return NULL;
    }

    int file_descriptor = fileno(file_pointer);
    if (flock(file_descriptor, LOCK_EX | LOCK_NB) == -1) {
        g_debug("database file is already locked by a different process: %s", file_path);

        fclose(file_pointer);
        file_pointer = NULL;
    }

    return file_pointer;
}

static bool
db_load_entry_shared(FILE *fp, struct FsearchDatabaseEntryCommon *shared, GString *previous_entry_name) {
    // name_offset: character position after which previous_entry_name and entry_name differ
    uint8_t name_offset = 0;
    if (fread(&name_offset, 1, 1, fp) != 1) {
        g_debug("failed to load name offset");
        return false;
    }

    // name_len: length of the new name characters
    uint8_t name_len = 0;
    if (fread(&name_len, 1, 1, fp) != 1) {
        g_debug("failed to load name length");
        return false;
    }

    // erase previous name starting at name_offset
    g_string_erase(previous_entry_name, name_offset, -1);

    char name[256] = "";
    // name: new characters to be appended to previous_entry_name
    if (name_len > 0) {
        if (fread(name, name_len, 1, fp) != 1) {
            g_debug("failed to load name");
            return false;
        }
        name[name_len] = '\0';
    }

    // now we can build the new full file name
    g_string_append(previous_entry_name, name);
    shared->name = g_strdup(previous_entry_name->str);

    // size: size of file/folder
    uint64_t size = 0;
    if (fread(&size, 8, 1, fp) != 1) {
        g_debug("failed to load size");
        return false;
    }
    shared->size = (off_t)size;

    return true;
}

static bool
db_load2(FsearchDatabase *db, const char *file_path) {
    assert(file_path != NULL);
    assert(db != NULL);

    FILE *fp = db_file_open_locked(file_path, "rb");
    if (!fp) {
        return false;
    }

    DynamicArray *folders = NULL;
    DynamicArray *files = NULL;
    GString *name_prev = g_string_sized_new(256);

    char magic[4];
    if (fread(magic, 4, 1, fp) != 1) {
        goto load_fail;
    }

    uint8_t majorver = 0;
    if (fread(&majorver, 1, 1, fp) != 1) {
        goto load_fail;
    }

    uint8_t minorver = 0;
    if (fread(&minorver, 1, 1, fp) != 1) {
        goto load_fail;
    }

    uint32_t num_folders = 0;
    if (fread(&num_folders, 4, 1, fp) != 1) {
        goto load_fail;
    }

    uint32_t num_files = 0;
    if (fread(&num_files, 4, 1, fp) != 1) {
        goto load_fail;
    }
    g_debug("load %d folders, %d files", num_folders, num_files);

    // pre-allocate the folders array so we can later map parent indices to the corresponding pointers
    folders = darray_new(num_folders);

    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = fsearch_memory_pool_malloc(db->folder_pool);
        folder->idx = i;
        folder->shared.type = DATABASE_ENTRY_TYPE_FOLDER;
        folder->shared.parent = NULL;
        darray_add_item(folders, folder);
    }

    // load folders
    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(folders, i);

        if (!db_load_entry_shared(fp, &folder->shared, name_prev)) {
            goto load_fail;
        }

        uint32_t parent_idx = 0;
        if (fread(&parent_idx, 4, 1, fp) != 1) {
            g_debug("failed to load parent_idx");
            goto load_fail;
        }
        if (parent_idx != folder->idx) {
            FsearchDatabaseEntryFolder *parent = darray_get_item(folders, parent_idx);
            folder->shared.parent = parent;
        }
        else {
            folder->shared.parent = NULL;
        }
    }

    g_string_erase(name_prev, 0, -1);

    files = darray_new(num_files);

    // load files
    for (uint32_t i = 0; i < num_files; i++) {
        FsearchDatabaseEntryFile *file = fsearch_memory_pool_malloc(db->file_pool);
        file->shared.type = DATABASE_ENTRY_TYPE_FILE;

        db_load_entry_shared(fp, &file->shared, name_prev);

        uint32_t parent_idx = 0;
        if (fread(&parent_idx, 4, 1, fp) != 1) {
            g_debug("failed to load parent_idx");
            goto load_fail;
        }
        FsearchDatabaseEntryFolder *parent = darray_get_item(folders, parent_idx);
        file->shared.parent = parent;

        darray_add_item(files, file);
    }

    g_string_free(name_prev, TRUE);
    name_prev = NULL;

    if (db->files) {
        darray_free(db->files);
    }
    db->files = files;
    if (db->folders) {
        darray_free(db->folders);
    }
    db->folders = folders;
    db->num_entries = num_files + num_folders;
    db->num_files = num_files;
    db->num_folders = num_folders;

    fclose(fp);
    fp = NULL;

    return true;

load_fail:
    g_debug("load fail");

    if (fp) {
        fclose(fp);
        fp = NULL;
    }

    if (folders) {
        darray_free(folders);
        folders = NULL;
    }

    if (files) {
        darray_free(files);
        files = NULL;
    }

    g_string_free(name_prev, TRUE);
    name_prev = NULL;

    return false;
}

static bool
db_save2(FsearchDatabase *db, const char *path) {
    assert(path != NULL);
    assert(db != NULL);

    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_debug("database path doesn't exist: %s", path);
        return false;
    }

    GString *path_full = g_string_new(path);
    g_string_append_c(path_full, G_DIR_SEPARATOR);
    g_string_append(path_full, "fsearch.db");

    GString *path_full_temp = g_string_new(path_full->str);
    g_string_append(path_full_temp, ".tmp");

    FILE *fp = db_file_open_locked(path_full_temp->str, "wb");
    if (!fp) {
        goto save_fail;
    }

    db_entry_update_folder_indices(db);

    const char magic[] = "FSDB";
    if (fwrite(magic, 4, 1, fp) != 1) {
        goto save_fail;
    }

    const uint8_t majorver = 0;
    if (fwrite(&majorver, 1, 1, fp) != 1) {
        goto save_fail;
    }

    const uint8_t minorver = 2;
    if (fwrite(&minorver, 1, 1, fp) != 1) {
        goto save_fail;
    }

    uint32_t num_folders = darray_get_num_items(db->folders);
    if (fwrite(&num_folders, 4, 1, fp) != 1) {
        goto save_fail;
    }

    uint32_t num_files = darray_get_num_items(db->files);
    if (fwrite(&num_files, 4, 1, fp) != 1) {
        goto save_fail;
    }

    GString *name_prev = g_string_sized_new(256);
    GString *name_new = g_string_sized_new(256);

    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(db->folders, i);
        if (!folder) {
            goto save_fail;
        }

        g_string_erase(name_new, 0, -1);
        g_string_append(name_new, folder->shared.name);

        uint8_t name_offset = get_name_offset(name_prev->str, name_new->str);
        if (fwrite(&name_offset, 1, 1, fp) != 1) {
            g_debug("failed to save name offset");
            goto save_fail;
        }
        uint8_t name_len = name_new->len - name_offset;
        if (fwrite(&name_len, 1, 1, fp) != 1) {
            g_debug("failed to save name length");
            goto save_fail;
        }
        g_string_erase(name_prev, name_offset, -1);
        g_string_append(name_prev, name_new->str + name_offset);

        if (name_len > 0) {
            const char *name = name_prev->str + name_offset;
            if (fwrite(name, name_len, 1, fp) != 1) {
                g_debug("failed to save name");
                goto save_fail;
            }
        }
        uint64_t size = folder->shared.size;
        if (fwrite(&size, 8, 1, fp) != 1) {
            g_debug("failed to save size");
            goto save_fail;
        }
        uint32_t parent_idx = folder->shared.parent ? folder->shared.parent->idx : folder->idx;
        // g_debug("%d: save folder: %s, %d", i, folder->shared.name, parent_idx);
        if (fwrite(&parent_idx, 4, 1, fp) != 1) {
            g_debug("failed to save parent_idx");
            goto save_fail;
        }
    }

    for (uint32_t i = 0; i < num_files; i++) {
        FsearchDatabaseEntryFile *file = darray_get_item(db->files, i);
        if (!file) {
            goto save_fail;
        }

        g_string_erase(name_new, 0, -1);
        g_string_append(name_new, file->shared.name);

        uint8_t name_offset = get_name_offset(name_prev->str, name_new->str);
        if (fwrite(&name_offset, 1, 1, fp) != 1) {
            g_debug("failed to save name offset");
            goto save_fail;
        }
        uint8_t name_len = name_new->len - name_offset;
        if (fwrite(&name_len, 1, 1, fp) != 1) {
            g_debug("failed to save name len");
            goto save_fail;
        }
        g_string_erase(name_prev, name_offset, -1);
        g_string_append(name_prev, name_new->str + name_offset);

        if (name_len > 0) {
            const char *name = name_prev->str + name_offset;
            if (fwrite(name, name_len, 1, fp) != 1) {
                g_debug("failed to save name");
                goto save_fail;
            }
        }

        uint64_t size = file->shared.size;
        if (fwrite(&size, 8, 1, fp) != 1) {
            g_debug("failed to save size");
            goto save_fail;
        }
        uint32_t parent_idx = file->shared.parent->idx;
        if (fwrite(&parent_idx, 4, 1, fp) != 1) {
            g_debug("failed to save parent_idx");
            goto save_fail;
        }
    }

    unlink(path_full->str);

    fclose(fp);
    fp = NULL;

    if (rename(path_full_temp->str, path_full->str) != 0) {
        goto save_fail;
    }

    g_string_free(path_full, TRUE);
    path_full = NULL;

    g_string_free(path_full_temp, TRUE);
    path_full_temp = NULL;

    return true;

save_fail:
    g_warning("save fail");

    if (fp) {
        fclose(fp);
        fp = NULL;
    }

    unlink(path_full_temp->str);
    unlink(path_full->str);

    g_string_free(path_full, TRUE);
    path_full = NULL;

    g_string_free(path_full_temp, TRUE);
    path_full_temp = NULL;

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

static void
db_entry_update_folder_size(FsearchDatabaseEntryFolder *folder, off_t size) {
    if (!folder) {
        return;
    }
    folder->shared.size += size;
    db_entry_update_folder_size(folder->shared.parent, size);
}

typedef struct DatabaseWalkContext {
    FsearchDatabase *db;
    GString *path;
    GTimer *timer;
    GCancellable *cancellable;
    void (*status_cb)(const char *);
    bool exclude_hidden;
} DatabaseWalkContext;

static int
db_folder_scan_recursive(DatabaseWalkContext *walk_context, FsearchDatabaseEntryFolder *parent) {
    if (walk_context->cancellable && g_cancellable_is_cancelled(walk_context->cancellable)) {
        return WALK_CANCEL;
    }

    GString *path = walk_context->path;
    g_string_append_c(path, G_DIR_SEPARATOR);

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
            g_debug("[database_scan] excluded directory: %s", path->str);
            continue;
        }

        if (is_dir) {
            FsearchDatabaseEntryFolder *folder_entry = fsearch_memory_pool_malloc(db->folder_pool);
            folder_entry->shared.name = strdup(dent->d_name);
            folder_entry->shared.parent = parent;
            folder_entry->shared.type = DATABASE_ENTRY_TYPE_FOLDER;

            parent->folder_children = g_slist_prepend(parent->folder_children, folder_entry);
            darray_add_item(db->folders, folder_entry);

            db->num_folders++;

            db_folder_scan_recursive(walk_context, folder_entry);
        }
        else {
            FsearchDatabaseEntryFile *file_entry = fsearch_memory_pool_malloc(db->file_pool);
            file_entry->shared.name = strdup(dent->d_name);
            file_entry->shared.parent = parent;
            file_entry->shared.type = DATABASE_ENTRY_TYPE_FILE;
            file_entry->shared.size = st.st_size;

            // update parent size
            db_entry_update_folder_size(parent, file_entry->shared.size);

            parent->file_children = g_slist_prepend(parent->file_children, file_entry);
            darray_add_item(db->files, file_entry);

            db->num_files++;
        }

        db->num_entries++;
    }

    closedir(dir);
    return WALK_OK;
}

static void
db_scan_folder(FsearchDatabase *db, const char *dname, GCancellable *cancellable, void (*status_cb)(const char *)) {
    assert(dname != NULL);
    assert(dname[0] == G_DIR_SEPARATOR);
    g_debug("[database] scan path: %s", dname);

    if (!g_file_test(dname, G_FILE_TEST_IS_DIR)) {
        g_warning("[database_scan] %s doesn't exist", dname);
        return;
    }

    GString *path = g_string_new(dname);
    // remove leading path separator '/' for root directory
    if (strcmp(path->str, G_DIR_SEPARATOR_S) == 0) {
        g_string_erase(path, 0, 1);
    }

    GTimer *timer = g_timer_new();
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
    parent->shared.name = strdup(path->str);
    parent->shared.parent = NULL;
    parent->shared.type = DATABASE_ENTRY_TYPE_FOLDER;
    darray_add_item(db->folders, parent);
    db->num_folders++;
    db->num_entries++;

    uint32_t res = db_folder_scan_recursive(&walk_context, parent);

    g_string_free(path, TRUE);
    g_timer_destroy(timer);
    if (res == WALK_OK) {
        g_debug("[database] scanned: %d files, %d files -> %d total", db->num_files, db->num_folders, db->num_entries);
        return;
    }

    g_warning("[database_scan] walk error: %d", res);
}

bool
db_save(FsearchDatabase *db) {
    assert(db != NULL);
    return false;
}

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
    db->file_pool = fsearch_memory_pool_new(BTREE_NODE_POOL_BLOCK_ELEMENTS,
                                            sizeof(FsearchDatabaseEntryFile),
                                            (GDestroyNotify)db_file_entry_destroy);
    db->folder_pool = fsearch_memory_pool_new(BTREE_NODE_POOL_BLOCK_ELEMENTS,
                                              sizeof(FsearchDatabaseEntryFolder),
                                              (GDestroyNotify)db_folder_entry_destroy);
    db->files = darray_new(1000);
    db->folders = darray_new(1000);
    db->exclude_hidden = exclude_hidden;
    db->ref_count = 1;
    return db;
}

static void
db_free(FsearchDatabase *db) {
    assert(db != NULL);

    g_debug("[database_free] freeing...");
    db_lock(db);
    if (db->ref_count > 0) {
        g_warning("[database_free] pending references on free: %d", db->ref_count);
    }

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

    g_debug("[database_free] freed");
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

bool
db_load(FsearchDatabase *db, const char *path, void (*status_cb)(const char *)) {
    assert(db != NULL);
    // return false;
    return db_load2(db, "/home/cb/testdir/fsearch.db");
}

bool
db_scan(FsearchDatabase *db, GCancellable *cancellable, void (*status_cb)(const char *)) {
    assert(db != NULL);

    bool ret = false;
    for (GList *l = db->includes; l != NULL; l = l->next) {
        FsearchIncludePath *fs_path = l->data;
        if (!fs_path->path) {
            continue;
        }
        if (!fs_path->enabled) {
            continue;
        }
        if (fs_path->update) {
            db_scan_folder(db, fs_path->path, cancellable, status_cb);
        }
    }
    g_debug("save database");
    db_sort(db);
    db_save2(db, "/home/cb/testdir");
    return ret;
}

void
db_ref(FsearchDatabase *db) {
    assert(db != NULL);
    db_lock(db);
    db->ref_count++;
    db_unlock(db);
    g_debug("[database_ref] increased to: %d", db->ref_count);
}

void
db_unref(FsearchDatabase *db) {
    assert(db != NULL);
    db_lock(db);
    db->ref_count--;
    db_unlock(db);
    g_debug("[database_unref] dropped to: %d", db->ref_count);
    if (db->ref_count <= 0) {
        db_free(db);
    }
}

static void
build_path_recursively(FsearchDatabaseEntryFolder *folder, GString *str) {
    if (!folder) {
        return;
    }
    if (folder->shared.parent) {
        build_path_recursively(folder->shared.parent, str);
        g_string_append_c(str, G_DIR_SEPARATOR);
    }
    if (strcmp(folder->shared.name, "") != 0) {
        g_string_append(str, folder->shared.name);
    }
}

GString *
db_entry_get_path(FsearchDatabaseEntry *entry) {
    GString *path = g_string_new(NULL);
    build_path_recursively(entry->shared.parent, path);
    return path;
}

GString *
db_entry_get_path_full(FsearchDatabaseEntry *entry) {
    GString *path_full = db_entry_get_path(entry);
    if (!path_full) {
        return NULL;
    }
    if (entry->shared.name[0] != G_DIR_SEPARATOR) {
        g_string_append_c(path_full, G_DIR_SEPARATOR);
    }
    g_string_append(path_full, entry->shared.name);
    return path_full;
}

void
db_entry_append_path(FsearchDatabaseEntry *entry, GString *str) {
    build_path_recursively(entry->shared.parent, str);
}

off_t
db_entry_get_size(FsearchDatabaseEntry *entry) {
    return entry ? entry->shared.size : 0;
}

const char *
db_entry_get_name(FsearchDatabaseEntry *entry) {
    if (!entry) {
        return NULL;
    }
    if (strcmp(entry->shared.name, "") != 0) {
        return entry->shared.name;
    }
    return G_DIR_SEPARATOR_S;
}

FsearchDatabaseEntryFolder *
db_entry_get_parent(FsearchDatabaseEntry *entry) {
    return entry ? entry->shared.parent : NULL;
}

FsearchDatabaseEntryType
db_entry_get_type(FsearchDatabaseEntry *entry) {
    return entry ? entry->shared.type : DATABASE_ENTRY_TYPE_NONE;
}
