#define G_LOG_DOMAIN "fsearch-result-view"

#include "fsearch_result_view.h"

#include "fsearch.h"
#include "fsearch_config.h"
#include "fsearch_database_view.h"
#include "fsearch_file_utils.h"

#include <gtk/gtk.h>
#include <math.h>

static int
get_icon_size_for_height(int height) {
    if (height < 24) {
        return 16;
    }
    if (height < 32) {
        return 24;
    }
    if (height < 48) {
        return 32;
    }
    return 48;
}

static cairo_surface_t *
get_icon_surface(GdkWindow *win,
                 const char *name,
                 FsearchDatabaseEntryType type,
                 const char *path,
                 int icon_size,
                 int scale_factor) {
    GtkIconTheme *icon_theme = gtk_icon_theme_get_default();
    if (!icon_theme) {
        return NULL;
    }

    cairo_surface_t *icon_surface = NULL;
    // GIcon *icon = fsearch_file_utils_get_icon_for_path(path);
    GIcon *icon = fsearch_file_utils_guess_icon(name, type == DATABASE_ENTRY_TYPE_FOLDER);
    const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    if (!names) {
        g_object_unref(icon);
        return NULL;
    }

    GtkIconInfo *icon_info = gtk_icon_theme_choose_icon_for_scale(icon_theme,
                                                                  (const char **)names,
                                                                  icon_size,
                                                                  scale_factor,
                                                                  GTK_ICON_LOOKUP_FORCE_SIZE);
    if (!icon_info) {
        return NULL;
    }

    GdkPixbuf *pixbuf = gtk_icon_info_load_icon(icon_info, NULL);
    if (pixbuf) {
        icon_surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale_factor, win);
        g_object_unref(pixbuf);
    }
    g_object_unref(icon);
    g_object_unref(icon_info);

    return icon_surface;
}

typedef struct {
    char *display_name;
    PangoAttrList *name_attr;
    PangoAttrList *path_attr;

    cairo_surface_t *icon_surface;

    GString *path;
    GString *full_path;
    char *size;
    char *type;
    char time[100];
} DrawRowContext;

static void
draw_row_ctx_init(FsearchDatabaseEntry *entry,
                  FsearchQuery *query,
                  GdkWindow *bin_window,
                  int icon_size,
                  DrawRowContext *ctx) {
    FsearchConfig *config = fsearch_application_get_config(FSEARCH_APPLICATION_DEFAULT);

    const char *name = db_entry_get_name(entry);
    if (!name) {
        return;
    }
    ctx->display_name = g_filename_display_name(name);

    ctx->name_attr = query ? fsearch_query_highlight_match(query, name) : NULL;

    ctx->path = db_entry_get_path(entry);
    if (query && ((query->has_separator && query->flags.auto_search_in_path) || query->flags.search_in_path)) {
        ctx->path_attr = fsearch_query_highlight_match(query, ctx->path->str);
    }

    ctx->full_path = g_string_new_len(ctx->path->str, ctx->path->len);
    g_string_append_c(ctx->full_path, G_DIR_SEPARATOR);
    g_string_append(ctx->full_path, name);

    ctx->type =
        fsearch_file_utils_get_file_type(name, db_entry_get_type(entry) == DATABASE_ENTRY_TYPE_FOLDER ? TRUE : FALSE);

    ctx->icon_surface = config->show_listview_icons ? get_icon_surface(bin_window,
                                                                       name,
                                                                       db_entry_get_type(entry),
                                                                       ctx->full_path->str,
                                                                       icon_size,
                                                                       gdk_window_get_scale_factor(bin_window))
                                                    : NULL;

    off_t size = db_entry_get_size(entry);
    ctx->size = fsearch_file_utils_get_size_formatted(size, config->show_base_2_units);

    const time_t mtime = db_entry_get_mtime(entry);
    strftime(ctx->time,
             100,
             "%Y-%m-%d %H:%M", //"%Y-%m-%d %H:%M",
             localtime(&mtime));
}

static void
draw_row_ctx_free(DrawRowContext *ctx) {
    if (ctx->display_name) {
        g_free(ctx->display_name);
        ctx->display_name = NULL;
    }
    if (ctx->type) {
        g_free(ctx->type);
        ctx->type = NULL;
    }
    if (ctx->size) {
        g_free(ctx->size);
        ctx->size = NULL;
    }
    if (ctx->path_attr) {
        pango_attr_list_unref(ctx->path_attr);
        ctx->path_attr = NULL;
    }
    if (ctx->name_attr) {
        pango_attr_list_unref(ctx->name_attr);
        ctx->name_attr = NULL;
    }
    if (ctx->path) {
        g_string_free(ctx->path, TRUE);
        ctx->path = NULL;
    }
    if (ctx->full_path) {
        g_string_free(ctx->full_path, TRUE);
        ctx->full_path = NULL;
    }
    if (ctx->icon_surface) {
        cairo_surface_destroy(ctx->icon_surface);
        ctx->icon_surface = NULL;
    }
}

char *
fsearch_result_view_query_tooltip(FsearchDatabaseEntry *entry,
                                  FsearchListViewColumn *col,
                                  PangoLayout *layout,
                                  uint32_t row_height) {
    FsearchConfig *config = fsearch_application_get_config(FSEARCH_APPLICATION_DEFAULT);

    const char *name = db_entry_get_name(entry);
    if (!name) {
        return NULL;
    }

    int width = col->effective_width - 2 * ROW_PADDING_X;
    char *text = NULL;

    switch (col->type) {
    case DATABASE_INDEX_TYPE_NAME:
        if (config->show_listview_icons) {
            int icon_size = get_icon_size_for_height(row_height - ROW_PADDING_X);
            width -= 2 * ROW_PADDING_X + icon_size;
        }
        text = g_filename_display_name(name);
        break;
    case DATABASE_INDEX_TYPE_PATH: {
        GString *path = db_entry_get_path(entry);
        text = g_filename_display_name(path->str);
        g_string_free(path, TRUE);
        path = NULL;
        break;
    }
    case DATABASE_INDEX_TYPE_FILETYPE: {
        text = fsearch_file_utils_get_file_type(name,
                                                db_entry_get_type(entry) == DATABASE_ENTRY_TYPE_FOLDER ? TRUE : FALSE);
        break;
    }
    case DATABASE_INDEX_TYPE_SIZE:
        text = fsearch_file_utils_get_size_formatted(db_entry_get_size(entry), config->show_base_2_units);
        break;
    case DATABASE_INDEX_TYPE_MODIFICATION_TIME: {
        const time_t mtime = db_entry_get_mtime(entry);
        char mtime_formatted[100] = "";
        strftime(mtime_formatted,
                 sizeof(mtime_formatted),
                 "%Y-%m-%d %H:%M", //"%Y-%m-%d %H:%M",
                 localtime(&mtime));
        text = g_strdup(mtime_formatted);
        break;
    }
    default:
        return NULL;
    }

    if (!text) {
        return NULL;
    }

    pango_layout_set_text(layout, text, -1);

    int layout_width = 0;
    pango_layout_get_pixel_size(layout, &layout_width, NULL);
    width -= layout_width;

    if (width < 0) {
        return text;
    }

    g_free(text);
    text = NULL;

    return NULL;
}

void
fsearch_result_view_draw_row(cairo_t *cr,
                             GdkWindow *bin_window,
                             PangoLayout *layout,
                             GtkStyleContext *context,
                             GList *columns,
                             cairo_rectangle_int_t *rect,
                             FsearchDatabaseEntry *entry,
                             FsearchQuery *query,
                             gboolean row_selected,
                             gboolean row_focused,
                             gboolean right_to_left_text) {
    if (!columns) {
        return;
    }

    FsearchConfig *config = fsearch_application_get_config(FSEARCH_APPLICATION_DEFAULT);

    const int icon_size = get_icon_size_for_height(rect->height - ROW_PADDING_X);

    DrawRowContext ctx = {};
    draw_row_ctx_init(entry, query, bin_window, icon_size, &ctx);

    GtkStateFlags flags = gtk_style_context_get_state(context);
    if (row_selected) {
        flags |= GTK_STATE_FLAG_SELECTED;
    }
    if (row_focused) {
        flags |= GTK_STATE_FLAG_FOCUSED;
    }

    gtk_style_context_save(context);
    gtk_style_context_set_state(context, flags);

    // Render row background
    gtk_render_background(context, cr, rect->x, rect->y, rect->width, rect->height);

    // Render row foreground
    uint32_t x = rect->x;
    for (GList *col = columns; col != NULL; col = col->next) {
        FsearchListViewColumn *column = col->data;
        if (!column->visible) {
            continue;
        }
        cairo_save(cr);
        cairo_rectangle(cr, x, rect->y, column->effective_width, rect->height);
        cairo_clip(cr);
        int dx = 0;
        int dw = 0;
        pango_layout_set_attributes(layout, NULL);
        switch (column->type) {
        case DATABASE_INDEX_TYPE_NAME: {
            if (config->show_listview_icons && ctx.icon_surface) {
                int x_icon = x;
                if (right_to_left_text) {
                    x_icon += column->effective_width - icon_size - ROW_PADDING_X;
                }
                else {
                    x_icon += ROW_PADDING_X;
                    dx += icon_size + 2 * ROW_PADDING_X;
                }
                dw += icon_size + 2 * ROW_PADDING_X;
                gtk_render_icon_surface(context,
                                        cr,
                                        ctx.icon_surface,
                                        x_icon,
                                        rect->y + floor((rect->height - icon_size) / 2.0));
            }
            pango_layout_set_attributes(layout, ctx.name_attr);
            pango_layout_set_text(layout, ctx.display_name, -1);
        } break;
        case DATABASE_INDEX_TYPE_PATH:
            pango_layout_set_attributes(layout, ctx.path_attr);
            pango_layout_set_text(layout, ctx.path->str, ctx.path->len);
            break;
        case DATABASE_INDEX_TYPE_SIZE:
            pango_layout_set_text(layout, ctx.size, -1);
            break;
        case DATABASE_INDEX_TYPE_FILETYPE:
            pango_layout_set_text(layout, ctx.type, -1);
            break;
        case DATABASE_INDEX_TYPE_MODIFICATION_TIME:
            pango_layout_set_text(layout, ctx.time, -1);
            break;
        default:
            pango_layout_set_text(layout, "Unknown column", -1);
        }

        pango_layout_set_width(layout, (column->effective_width - 2 * ROW_PADDING_X - dw) * PANGO_SCALE);
        pango_layout_set_alignment(layout, column->alignment);
        pango_layout_set_ellipsize(layout, column->ellipsize_mode);
        gtk_render_layout(context, cr, x + ROW_PADDING_X + dx, rect->y + ROW_PADDING_Y, layout);
        x += column->effective_width;
        cairo_restore(cr);
    }
    gtk_style_context_restore(context);

    draw_row_ctx_free(&ctx);
}
