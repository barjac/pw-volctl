/*
 * fdv-volctl.c
 * FreeDV Volume Control - GTK4/C
 *
 * Reads ~/.config/pipewire/saved-volumes.db, presents a filtered device list
 * with a slider. Adjustments call wpctl directly and update the db file.
 *
 * Build:
 *   gcc $(pkg-config --cflags gtk4) -o fdv-volctl fdv-volctl.c $(pkg-config --libs gtk4)
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_DEVICES     64
#define MAX_NAME_LEN   128
#define MAX_CLASS_LEN   64
#define DB_PATH        "%s/.config/pipewire/saved-volumes.db"

/* ── Data model ─────────────────────────────────────────────────────────── */

typedef struct {
    char   name[MAX_NAME_LEN];
    char   class[MAX_CLASS_LEN];
    double vol;          /* 0.0 – 1.0 */
} Device;

static Device  devices[MAX_DEVICES];
static int     n_devices = 0;
static char    db_path[256];

/* ── Application state ───────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *window;
    GtkWidget *list_box;
    GtkWidget *lbl_name;
    GtkWidget *lbl_class;
    GtkWidget *lbl_pct;
    GtkWidget *slider;
    GtkWidget *toggle_btn;
    gboolean   show_all;
    int        selected_idx;  /* -1 if none */
} AppData;

/* ── DB helpers ──────────────────────────────────────────────────────────── */

static void build_db_path(void) {
    const char *home = g_get_home_dir();
    snprintf(db_path, sizeof(db_path), DB_PATH, home);
}

static int load_db(void) {
    n_devices = 0;
    FILE *f = fopen(db_path, "r");
    if (!f) {
        g_warning("Cannot open db file: %s", db_path);
        return 0;
    }
    char line[512];
    while (fgets(line, sizeof(line), f) && n_devices < MAX_DEVICES) {
        /* strip newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        char *p1 = strchr(line, '|');
        if (!p1) continue;
        char *p2 = strchr(p1 + 1, '|');
        if (!p2) continue;

        *p1 = '\0';
        *p2 = '\0';

        g_strlcpy(devices[n_devices].name,  line,    MAX_NAME_LEN);
        g_strlcpy(devices[n_devices].class, p1 + 1,  MAX_CLASS_LEN);
        devices[n_devices].vol = atof(p2 + 1);
        n_devices++;
    }
    fclose(f);
    return n_devices;
}

static void save_db(void) {
    FILE *f = fopen(db_path, "w");
    if (!f) {
        g_warning("Cannot write db file: %s", db_path);
        return;
    }
    for (int i = 0; i < n_devices; i++) {
        fprintf(f, "%s|%s|%.2f\n",
                devices[i].name,
                devices[i].class,
                devices[i].vol);
    }
    fclose(f);
}

/* ── wpctl integration ───────────────────────────────────────────────────── */

/* Returns the wpctl numeric ID for a given node name+class, or -1 if not found.
   Uses the same two-pass logic as pw-restore-vols. */
static int find_wpctl_id(const char *name, const char *class) {
    FILE *fp = popen("wpctl status", "r");
    if (!fp) return -1;

    /* Collect all candidate IDs from lines containing [vol: */
    int ids[256];
    int n_ids = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && n_ids < 256) {
        if (!strstr(line, "[vol:")) continue;
        /* strip box-drawing chars */
        char clean[512];
        int ci = 0;
        for (int i = 0; line[i] && ci < 510; i++) {
            unsigned char c = (unsigned char)line[i];
            /* keep ASCII printable */
            if (c < 0x80 && (c >= 0x20 || c == '\t')) clean[ci++] = line[i];
        }
        clean[ci] = '\0';
        /* first token before '.' is the ID */
        char *dot = strchr(clean, '.');
        if (!dot) continue;
        char id_str[16] = {0};
        int len = dot - clean;
        if (len <= 0 || len >= (int)sizeof(id_str)) continue;
        memcpy(id_str, clean, len);
        id_str[len] = '\0';
        /* strip spaces */
        char *s = id_str;
        while (*s == ' ' || *s == '*') s++;
        if (*s < '0' || *s > '9') continue;
        ids[n_ids++] = atoi(s);
    }
    pclose(fp);

    /* Inspect each candidate */
    for (int i = 0; i < n_ids; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "wpctl inspect %d 2>/dev/null", ids[i]);
        FILE *fp2 = popen(cmd, "r");
        if (!fp2) continue;

        char node_name[MAX_NAME_LEN]  = {0};
        char node_class[MAX_CLASS_LEN] = {0};
        while (fgets(line, sizeof(line), fp2)) {
            if (strstr(line, "node.name")) {
                char *q1 = strchr(line, '"');
                if (q1) { char *q2 = strchr(q1+1, '"'); if (q2) { *q2='\0'; g_strlcpy(node_name, q1+1, MAX_NAME_LEN); } }
            }
            if (strstr(line, "media.class")) {
                char *q1 = strchr(line, '"');
                if (q1) { char *q2 = strchr(q1+1, '"'); if (q2) { *q2='\0'; g_strlcpy(node_class, q1+1, MAX_CLASS_LEN); } }
            }
        }
        pclose(fp2);

        if (strcmp(node_name, name) == 0 && strcmp(node_class, class) == 0)
            return ids[i];
    }
    return -1;
}

static void apply_volume(int device_idx) {
    if (device_idx < 0 || device_idx >= n_devices) return;
    Device *d = &devices[device_idx];

    int id = find_wpctl_id(d->name, d->class);
    if (id < 0) {
        g_warning("Could not find wpctl ID for %s", d->name);
        return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume %d %.2f", id, d->vol);
    int ret = system(cmd);
    if (ret != 0)
        g_warning("wpctl set-volume failed for %s", d->name);

    save_db();
}

/* ── GUI helpers ─────────────────────────────────────────────────────────── */

/* Friendly short name for display */
static void short_name(const char *full, char *out, int outlen) {
    /* If it starts with "FDV_" show as-is */
    if (g_str_has_prefix(full, "FDV_") || g_str_has_prefix(full, "FF_")) {
        g_strlcpy(out, full, outlen);
        return;
    }
    /* For alsa names like alsa_output.usb-headset.analog-stereo
       extract the middle part: usb-headset */
    const char *dot1 = strchr(full, '.');
    if (!dot1) { g_strlcpy(out, full, outlen); return; }
    dot1++;
    const char *dot2 = strchr(dot1, '.');
    if (!dot2) { g_strlcpy(out, dot1, outlen); return; }
    int len = dot2 - dot1;
    if (len >= outlen) len = outlen - 1;
    strncpy(out, dot1, len);
    out[len] = '\0';
}

static void populate_list(AppData *app) {
    /* Remove all existing rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->list_box)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(app->list_box), child);

    for (int i = 0; i < n_devices; i++) {
        if (!app->show_all && devices[i].vol >= 0.995) continue;

        char sname[MAX_NAME_LEN];
        short_name(devices[i].name, sname, sizeof(sname));

        char label[256];
        int pct = (int)round(devices[i].vol * 100.0);
        snprintf(label, sizeof(label), "%s  [%d%%]", sname, pct);

        GtkWidget *row_lbl = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(row_lbl), 0.0f);
        gtk_widget_set_margin_start(row_lbl, 8);
        gtk_widget_set_margin_end(row_lbl, 8);
        gtk_widget_set_margin_top(row_lbl, 4);
        gtk_widget_set_margin_bottom(row_lbl, 4);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_lbl);
        /* Store device index as row data */
        g_object_set_data(G_OBJECT(row), "device-idx", GINT_TO_POINTER(i));

        gtk_list_box_append(GTK_LIST_BOX(app->list_box), row);
    }
}

static void update_right_panel(AppData *app, int idx) {
    app->selected_idx = idx;
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->lbl_name),  "No device selected");
        gtk_label_set_text(GTK_LABEL(app->lbl_class), "");
        gtk_label_set_text(GTK_LABEL(app->lbl_pct),   "");
        gtk_widget_set_sensitive(app->slider, FALSE);
        return;
    }
    Device *d = &devices[idx];
    char sname[MAX_NAME_LEN];
    short_name(d->name, sname, sizeof(sname));
    gtk_label_set_text(GTK_LABEL(app->lbl_name),  sname);
    gtk_label_set_text(GTK_LABEL(app->lbl_class), d->class);

    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", (int)round(d->vol * 100.0));
    gtk_label_set_text(GTK_LABEL(app->lbl_pct), pct_str);

    /* Block signal temporarily while we set value */
    g_signal_handlers_block_matched(app->slider,
        G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, app);
    gtk_range_set_value(GTK_RANGE(app->slider), d->vol * 100.0);
    g_signal_handlers_unblock_matched(app->slider,
        G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, app);

    gtk_widget_set_sensitive(app->slider, TRUE);
}

/* ── Signal callbacks ────────────────────────────────────────────────────── */

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    (void)lb;
    if (!row) { update_right_panel(app, -1); return; }
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "device-idx"));
    update_right_panel(app, idx);
}

static void on_slider_value_changed(GtkRange *range, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (app->selected_idx < 0) return;

    double val = gtk_range_get_value(range) / 100.0;
    devices[app->selected_idx].vol = val;

    /* Update percentage label live */
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", (int)round(val * 100.0));
    gtk_label_set_text(GTK_LABEL(app->lbl_pct), pct_str);

    /* Update the list row label */
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->list_box));
    if (row) {
        GtkWidget *lbl = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        char sname[MAX_NAME_LEN];
        short_name(devices[app->selected_idx].name, sname, sizeof(sname));
        char label[256];
        snprintf(label, sizeof(label), "%s  [%d%%]", sname, (int)round(val*100.0));
        gtk_label_set_text(GTK_LABEL(lbl), label);
    }
}

/* Called when user releases the slider — apply and save */
static gboolean on_slider_released(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget; (void)event;
    AppData *app = (AppData *)user_data;
    apply_volume(app->selected_idx);
    return FALSE;
}

/* Also apply on key release (arrow keys on slider) */
static gboolean on_slider_key_released(GtkEventController *ctrl, guint keyval,
                                        guint keycode, GdkModifierType state,
                                        gpointer user_data) {
    (void)ctrl; (void)keycode; (void)state;
    AppData *app = (AppData *)user_data;
    if (keyval == GDK_KEY_Left  || keyval == GDK_KEY_Right ||
        keyval == GDK_KEY_Up    || keyval == GDK_KEY_Down  ||
        keyval == GDK_KEY_Home  || keyval == GDK_KEY_End)
        apply_volume(app->selected_idx);
    return FALSE;
}

static void on_toggle_show_all(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    app->show_all = !app->show_all;
    gtk_button_set_label(GTK_BUTTON(app->toggle_btn),
        app->show_all ? "Show active only" : "Show all devices");
    populate_list(app);
    update_right_panel(app, -1);
}

static void on_reload(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    load_db();
    populate_list(app);
    update_right_panel(app, -1);
}

/* ── CSS ─────────────────────────────────────────────────────────────────── */

static const char *APP_CSS =
    "window {"
    "  background-color: #2b2b2b;"
    "}"
    ".panel {"
    "  background-color: #333333;"
    "  border-radius: 6px;"
    "}"
    ".device-name {"
    "  font-size: 15px;"
    "  font-weight: bold;"
    "  color: #e0e0e0;"
    "}"
    ".device-class {"
    "  font-size: 11px;"
    "  color: #888888;"
    "}"
    ".pct-label {"
    "  font-size: 24px;"
    "  font-weight: bold;"
    "  color: #4fc3f7;"
    "}"
    "list {"
    "  background-color: #2b2b2b;"
    "}"
    "row {"
    "  color: #cccccc;"
    "  font-size: 12px;"
    "}"
    "row:selected {"
    "  background-color: #1565c0;"
    "  color: #ffffff;"
    "}"
    "scale trough {"
    "  background-color: #555555;"
    "  min-height: 8px;"
    "}"
    "scale highlight {"
    "  background-color: #4fc3f7;"
    "}"
    "button {"
    "  background: #444444;"
    "  color: #e0e0e0;"
    "  border: none;"
    "  border-radius: 4px;"
    "  padding: 6px 12px;"
    "}"
    "button:hover {"
    "  background: #555555;"
    "}";

/* ── App activate ────────────────────────────────────────────────────────── */

static void activate(GtkApplication *gtk_app, gpointer user_data) {
    (void)user_data;

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, APP_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    AppData *app = g_new0(AppData, 1);
    app->show_all     = FALSE;
    app->selected_idx = -1;

    build_db_path();
    load_db();

    /* ── Window ── */
    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "FreeDV Volume Control");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 520, 320);
    gtk_window_set_resizable(GTK_WINDOW(app->window), FALSE);

    /* ── Root hbox ── */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 10);
    gtk_widget_set_margin_end(hbox, 10);
    gtk_widget_set_margin_top(hbox, 10);
    gtk_widget_set_margin_bottom(hbox, 10);
    gtk_window_set_child(GTK_WINDOW(app->window), hbox);

    /* ── Left panel (list + buttons) ── */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(left_vbox, FALSE);
    gtk_widget_set_size_request(left_vbox, 240, -1);
    gtk_box_append(GTK_BOX(hbox), left_vbox);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(left_vbox), scroll);

    app->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->list_box),
        GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->list_box);
    g_signal_connect(app->list_box, "row-selected",
        G_CALLBACK(on_row_selected), app);

    /* Toggle button */
    app->toggle_btn = gtk_button_new_with_label("Show all devices");
    gtk_box_append(GTK_BOX(left_vbox), app->toggle_btn);
    g_signal_connect(app->toggle_btn, "clicked",
        G_CALLBACK(on_toggle_show_all), app);

    /* Reload button */
    GtkWidget *reload_btn = gtk_button_new_with_label("Reload from file");
    gtk_box_append(GTK_BOX(left_vbox), reload_btn);
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(on_reload), app);

    /* ── Right panel ── */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_hexpand(right_vbox, TRUE);
    gtk_widget_set_valign(right_vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(right_vbox, 16);
    gtk_box_append(GTK_BOX(hbox), right_vbox);

    app->lbl_name = gtk_label_new("No device selected");
    gtk_widget_add_css_class(app->lbl_name, "device-name");
    gtk_label_set_wrap(GTK_LABEL(app->lbl_name), TRUE);
    gtk_label_set_xalign(GTK_LABEL(app->lbl_name), 0.0f);
    gtk_box_append(GTK_BOX(right_vbox), app->lbl_name);

    app->lbl_class = gtk_label_new("");
    gtk_widget_add_css_class(app->lbl_class, "device-class");
    gtk_label_set_xalign(GTK_LABEL(app->lbl_class), 0.0f);
    gtk_box_append(GTK_BOX(right_vbox), app->lbl_class);

    app->lbl_pct = gtk_label_new("");
    gtk_widget_add_css_class(app->lbl_pct, "pct-label");
    gtk_label_set_xalign(GTK_LABEL(app->lbl_pct), 0.0f);
    gtk_box_append(GTK_BOX(right_vbox), app->lbl_pct);

    /* Slider */
    app->slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
        0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(app->slider), FALSE);
    gtk_widget_set_sensitive(app->slider, FALSE);
    gtk_widget_set_hexpand(app->slider, TRUE);
    gtk_box_append(GTK_BOX(right_vbox), app->slider);

    /* Add tick marks at 0, 25, 50, 75, 100 */
    gtk_scale_add_mark(GTK_SCALE(app->slider),   0, GTK_POS_BOTTOM, "0%");
    gtk_scale_add_mark(GTK_SCALE(app->slider),  25, GTK_POS_BOTTOM, NULL);
    gtk_scale_add_mark(GTK_SCALE(app->slider),  50, GTK_POS_BOTTOM, "50%");
    gtk_scale_add_mark(GTK_SCALE(app->slider),  75, GTK_POS_BOTTOM, NULL);
    gtk_scale_add_mark(GTK_SCALE(app->slider), 100, GTK_POS_BOTTOM, "100%");

    g_signal_connect(app->slider, "value-changed",
        G_CALLBACK(on_slider_value_changed), app);

    /* Apply on button release */
    GtkEventController *motion = gtk_event_controller_legacy_new();
    g_signal_connect(motion, "event",
        G_CALLBACK(on_slider_released), app);
    gtk_widget_add_controller(app->slider, motion);

    /* Apply on keyboard */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-released",
        G_CALLBACK(on_slider_key_released), app);
    gtk_widget_add_controller(app->slider, key_ctrl);

    populate_list(app);
    gtk_window_present(GTK_WINDOW(app->window));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new(
        "uk.radio.fdv-volctl",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
