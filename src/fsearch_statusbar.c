#include "fsearch_statusbar.h"
#include "fsearch.h"

#include <glib/gi18n.h>

struct _FsearchStatusbar {
    GtkRevealer parent_instance;

    GtkWidget *statusbar_database_stack;
    GtkWidget *statusbar_database_status_box;
    GtkWidget *statusbar_database_status_label;
    GtkWidget *statusbar_database_updating_box;
    GtkWidget *statusbar_database_updating_label;
    GtkWidget *statusbar_database_updating_spinner;
    GtkWidget *statusbar_match_case_revealer;
    GtkWidget *statusbar_scan_label;
    GtkWidget *statusbar_scan_status_label;
    GtkWidget *statusbar_search_filter_revealer;
    GtkWidget *statusbar_search_in_path_revealer;
    GtkWidget *statusbar_search_filter_label;
    GtkWidget *statusbar_search_label;
    GtkWidget *statusbar_search_mode_revealer;
    GtkWidget *statusbar_selection_num_files_label;
    GtkWidget *statusbar_selection_num_folders_label;
    GtkWidget *statusbar_selection_revealer;
    GtkWidget *statusbar_smart_case_revealer;
    GtkWidget *statusbar_smart_path_revealer;

    guint statusbar_timeout_id;
};

G_DEFINE_TYPE(FsearchStatusbar, fsearch_statusbar, GTK_TYPE_REVEALER)

static void
statusbar_remove_update_cb(FsearchStatusbar *sb) {
    if (sb->statusbar_timeout_id) {
        g_source_remove(sb->statusbar_timeout_id);
        sb->statusbar_timeout_id = 0;
    }
}

void
fsearch_statusbar_set_query_text(FsearchStatusbar *sb, const char *text) {
    statusbar_remove_update_cb(sb);
    gtk_label_set_text(GTK_LABEL(sb->statusbar_search_label), text);
}

static gboolean
statusbar_set_query_status(gpointer user_data) {
    FsearchStatusbar *sb = user_data;
    gtk_label_set_text(GTK_LABEL(sb->statusbar_search_label), _("Querying…"));
    sb->statusbar_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

void
fsearch_statusbar_set_query_status_delayed(FsearchStatusbar *sb) {
    statusbar_remove_update_cb(sb);
    sb->statusbar_timeout_id = g_timeout_add(200, statusbar_set_query_status, sb);
}

void
fsearch_statusbar_set_revealer_visibility(FsearchStatusbar *sb, FsearchStatusbarRevealer revealer, gboolean visible) {
    GtkRevealer *r = NULL;
    switch (revealer) {
    case FSEARCH_STATUSBAR_REVEALER_MATCH_CASE:
        r = GTK_REVEALER(sb->statusbar_match_case_revealer);
        break;
    case FSEARCH_STATUSBAR_REVEALER_SMART_MATCH_CASE:
        r = GTK_REVEALER(sb->statusbar_smart_case_revealer);
        break;
    case FSEARCH_STATUSBAR_REVEALER_SEARCH_IN_PATH:
        r = GTK_REVEALER(sb->statusbar_search_in_path_revealer);
        break;
    case FSEARCH_STATUSBAR_REVEALER_SMART_SEARCH_IN_PATH:
        r = GTK_REVEALER(sb->statusbar_smart_path_revealer);
        break;
    case FSEARCH_STATUSBAR_REVEALER_REGEX:
        r = GTK_REVEALER(sb->statusbar_search_mode_revealer);
        break;
    default:
        g_debug("unknown revealer");
    }
    if (r) {
        gtk_revealer_set_reveal_child(r, visible);
    }
}

void
fsearch_statusbar_set_filter(FsearchStatusbar *sb, const char *filter_name) {
    gtk_label_set_text(GTK_LABEL(sb->statusbar_search_filter_label), filter_name);
    gtk_revealer_set_reveal_child(GTK_REVEALER(sb->statusbar_search_filter_revealer), filter_name ? TRUE : FALSE);
}

void
fsearch_statusbar_set_database_indexing_state(FsearchStatusbar *sb, const char *text) {
    if (!text) {
        gtk_widget_hide(sb->statusbar_scan_label);
        gtk_widget_hide(sb->statusbar_scan_status_label);
    }
    else {
        gtk_widget_show(sb->statusbar_scan_label);
        gtk_widget_show(sb->statusbar_scan_status_label);
        gtk_label_set_text(GTK_LABEL(sb->statusbar_scan_status_label), text);
    }
}

void
fsearch_statusbar_set_selection(FsearchStatusbar *sb,
                                uint32_t num_files_selected,
                                uint32_t num_folders_selected,
                                uint32_t num_files,
                                uint32_t num_folders) {
    if (!num_folders_selected && !num_files_selected) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(sb->statusbar_selection_revealer), FALSE);
    }
    else {
        gtk_revealer_set_reveal_child(GTK_REVEALER(sb->statusbar_selection_revealer), TRUE);
        char text[100] = "";
        snprintf(text, sizeof(text), "%d/%d", num_folders_selected, num_folders);
        gtk_label_set_text(GTK_LABEL(sb->statusbar_selection_num_folders_label), text);
        snprintf(text, sizeof(text), "%d/%d", num_files_selected, num_files);
        gtk_label_set_text(GTK_LABEL(sb->statusbar_selection_num_files_label), text);
    }
}

void
fsearch_statusbar_set_database_loading(FsearchStatusbar *sb) {
    gtk_stack_set_visible_child(GTK_STACK(sb->statusbar_database_stack), sb->statusbar_database_updating_box);
    gtk_spinner_start(GTK_SPINNER(sb->statusbar_database_updating_spinner));
    gchar db_text[100] = "";
    snprintf(db_text, sizeof(db_text), _("Loading Database…"));
    gtk_label_set_text(GTK_LABEL(sb->statusbar_database_updating_label), db_text);
}

void
fsearch_statusbar_set_database_scanning(FsearchStatusbar *sb) {
    gtk_stack_set_visible_child(GTK_STACK(sb->statusbar_database_stack), sb->statusbar_database_updating_box);
    gtk_spinner_start(GTK_SPINNER(sb->statusbar_database_updating_spinner));
    gchar db_text[100] = "";
    snprintf(db_text, sizeof(db_text), _("Updating Database…"));
    gtk_label_set_text(GTK_LABEL(sb->statusbar_database_updating_label), db_text);
}

void
fsearch_statusbar_set_database_idle(FsearchStatusbar *sb, uint32_t num_files, uint32_t num_folders) {
    fsearch_statusbar_set_query_text(sb, "");

    gtk_spinner_stop(GTK_SPINNER(sb->statusbar_database_updating_spinner));
    gtk_widget_hide(sb->statusbar_scan_label);
    gtk_widget_hide(sb->statusbar_scan_status_label);

    gtk_stack_set_visible_child(GTK_STACK(sb->statusbar_database_stack), sb->statusbar_database_status_box);
    gchar db_text[100] = "";
    snprintf(db_text, sizeof(db_text), _("%'d Items"), num_files + num_folders);
    gtk_label_set_text(GTK_LABEL(sb->statusbar_database_status_label), db_text);
}

static void
statusbar_scan_started_cb(gpointer data, gpointer user_data) {
    FsearchStatusbar *statusbar = FSEARCH_STATUSBAR(user_data);
    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    FsearchConfig *config = fsearch_application_get_config(app);

    if (config->show_indexing_status) {
        gtk_widget_show(statusbar->statusbar_scan_label);
        gtk_widget_show(statusbar->statusbar_scan_status_label);
    }
    fsearch_statusbar_set_database_scanning(statusbar);
}

static void
statusbar_load_started_cb(gpointer data, gpointer user_data) {
    FsearchStatusbar *statusbar = FSEARCH_STATUSBAR(user_data);
    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    FsearchConfig *config = fsearch_application_get_config(app);
    if (config->show_indexing_status) {
        gtk_widget_show(statusbar->statusbar_scan_label);
        gtk_widget_show(statusbar->statusbar_scan_status_label);
    }
    fsearch_statusbar_set_database_loading(statusbar);
}

static void
statusbar_update_finished_cb(gpointer data, gpointer user_data) {
    FsearchStatusbar *statusbar = FSEARCH_STATUSBAR(user_data);
    FsearchDatabase *db = fsearch_application_get_db(FSEARCH_APPLICATION_DEFAULT);
    fsearch_statusbar_set_database_idle(statusbar, db ? db_get_num_files(db) : 0, db ? db_get_num_folders(db) : 0);
    db_unref(db);
}

static gboolean
toggle_action_on_2button_press(GdkEvent *event, const char *action, gpointer user_data) {
    guint button;
    gdk_event_get_button(event, &button);
    GdkEventType type = gdk_event_get_event_type(event);
    if (button != GDK_BUTTON_PRIMARY || type != GDK_2BUTTON_PRESS) {
        return FALSE;
    }
    GtkWidget *widget = GTK_WIDGET(user_data);
    GActionGroup *group = gtk_widget_get_action_group(widget, "win");
    if (!group) {
        return FALSE;
    }
    GVariant *state = g_action_group_get_action_state(group, action);
    g_action_group_change_action_state(group, action, g_variant_new_boolean(!g_variant_get_boolean(state)));
    g_variant_unref(state);
    return TRUE;
}

static gboolean
on_search_filter_label_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    guint button;
    gdk_event_get_button(event, &button);
    GdkEventType type = gdk_event_get_event_type(event);
    if (button != GDK_BUTTON_PRIMARY || type != GDK_2BUTTON_PRESS) {
        return FALSE;
    }
    GActionGroup *group = gtk_widget_get_action_group(widget, "win");
    if (!group) {
        return FALSE;
    }
    GVariant *state = g_action_group_get_action_state(group, "filter");
    g_action_group_change_action_state(group, "filter", g_variant_new_int32(0));
    g_variant_unref(state);
    return TRUE;
}

static gboolean
on_search_mode_label_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    return toggle_action_on_2button_press(event, "search_mode", widget);
}

static gboolean
on_search_in_path_label_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    return toggle_action_on_2button_press(event, "search_in_path", widget);
}

static gboolean
on_match_case_label_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    return toggle_action_on_2button_press(event, "match_case", widget);
}

static void
fsearch_statusbar_init(FsearchStatusbar *self) {
    g_assert(FSEARCH_IS_STATUSBAR(self));

    gtk_widget_init_template(GTK_WIDGET(self));

    gtk_spinner_stop(GTK_SPINNER(self->statusbar_database_updating_spinner));

    gtk_stack_set_visible_child(GTK_STACK(self->statusbar_database_stack), self->statusbar_database_status_box);
    FsearchDatabase *db = fsearch_application_get_db(FSEARCH_APPLICATION_DEFAULT);

    uint32_t num_items = 0;
    if (db) {
        num_items = db_get_num_entries(db);
        db_unref(db);
    }

    fsearch_statusbar_set_selection(self, 0, 0, 0, 0);

    gchar db_text[100] = "";
    snprintf(db_text, sizeof(db_text), _("%'d Items"), num_items);
    gtk_label_set_text(GTK_LABEL(self->statusbar_database_status_label), db_text);

    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    g_signal_connect_object(app, "database-scan-started", G_CALLBACK(statusbar_scan_started_cb), self, G_CONNECT_AFTER);
    g_signal_connect_object(app,
                            "database-update-finished",
                            G_CALLBACK(statusbar_update_finished_cb),
                            self,
                            G_CONNECT_AFTER);
    g_signal_connect_object(app, "database-load-started", G_CALLBACK(statusbar_load_started_cb), self, G_CONNECT_AFTER);
}

static void
fsearch_statusbar_class_init(FsearchStatusbarClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    // object_class->constructed = fsearch_application_window_constructed;
    // object_class->finalize = fsearch_application_window_finalize;

    gtk_widget_class_set_template_from_resource(widget_class, "/io/github/cboxdoerfer/fsearch/ui/statusbar.glade");
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_database_stack);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_database_status_box);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_database_status_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_database_updating_box);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_database_updating_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_database_updating_spinner);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_match_case_revealer);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_scan_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_scan_status_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_search_filter_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_search_filter_revealer);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_search_in_path_revealer);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_search_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_search_mode_revealer);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_selection_num_files_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_selection_num_folders_label);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_selection_revealer);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_smart_case_revealer);
    gtk_widget_class_bind_template_child(widget_class, FsearchStatusbar, statusbar_smart_path_revealer);

    gtk_widget_class_bind_template_callback(widget_class, on_match_case_label_button_press_event);
    gtk_widget_class_bind_template_callback(widget_class, on_search_filter_label_button_press_event);
    gtk_widget_class_bind_template_callback(widget_class, on_search_in_path_label_button_press_event);
    gtk_widget_class_bind_template_callback(widget_class, on_search_mode_label_button_press_event);
}

FsearchStatusbar *
fsearch_statusbar_new() {
    return g_object_new(FSEARCH_STATUSBAR_TYPE, NULL, NULL, NULL);
}
