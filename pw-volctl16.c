/*
 * pw-volctl.c
 * PipeWire Volume Control - GTK4/C
 *
 * Reads the last-used preset file on startup (if any), otherwise reads live
 * volumes from wpctl. Slider changes apply in real time via wpctl.
 * Revert restores the in-memory snapshot of the last loaded/saved file.
 * Save / Load use a file chooser for named presets.
 *
 * Build:
 *   gcc $(pkg-config --cflags gtk4) -o pw-volctl pw-volctl.c $(pkg-config --libs gtk4)
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_DEVICES    64
#define MAX_NAME_LEN  128
#define MAX_CLASS_LEN  64
#define TEMP_PATH     "%s/.config/pipewire/pw-volctl.tmp"
#define LAST_PATH     "%s/.config/pipewire/pw-volctl.last"
#define DEFAULT_DIR   "%s/.config/pipewire"

/* ── Data model ─────────────────────────────────────────────────────────── */

typedef struct {
    char   name[MAX_NAME_LEN];
    char   class[MAX_CLASS_LEN];
    int    wpctl_id;
    double vol;
} Device;

/* Live working set */
static Device devices[MAX_DEVICES];
static int    n_devices = 0;

/* Snapshot of the last loaded/saved file — used by Revert */
static Device snapshot[MAX_DEVICES];
static int    n_snapshot = 0;

static char   temp_path[256];
static char   last_path[256];
static char   default_dir[256];

/* ── Application state ───────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *window;
    GtkWidget *list_box;
    GtkWidget *lbl_name;
    GtkWidget *lbl_class;
    GtkWidget *lbl_pct;
    GtkWidget *btn_dec5;
    GtkWidget *btn_dec1;
    GtkWidget *btn_inc1;
    GtkWidget *btn_inc5;
    GtkWidget *toggle_btn;
    GtkWidget *revert_btn;
    GtkWidget *status_lbl;
    gboolean   show_all;
    gboolean   unsaved;
    int        selected_idx;
    char       current_file[512];
} AppData;

/* ── Forward declarations ────────────────────────────────────────────────── */
static int   load_file(const char *path);
static void  write_file(const char *path);
static void  save_temp(void);
static void  apply_volume(int idx);
static void  apply_all_volumes(void);
static void  populate_list(AppData *app);
static void  update_right_panel(AppData *app, int idx);
static void  set_unsaved(AppData *app, gboolean unsaved);
static void  set_current_file(AppData *app, const char *path);
static void  write_last_file(const char *path);
static void  take_snapshot(void);
static void  restore_snapshot(void);
static void  adjust_volume(AppData *app, double delta);
static void  set_step_buttons_sensitive(AppData *app, gboolean sensitive);

/* ── Path helpers ────────────────────────────────────────────────────────── */

static void build_paths(void) {
    const char *home = g_get_home_dir();
    snprintf(temp_path,   sizeof(temp_path),   TEMP_PATH,   home);
    snprintf(last_path,   sizeof(last_path),   LAST_PATH,   home);
    snprintf(default_dir, sizeof(default_dir), DEFAULT_DIR, home);
}

static char *read_last_file(void) {
    FILE *f = fopen(last_path, "r");
    if (!f) return NULL;
    char buf[512] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] ? g_strdup(buf) : NULL;
}

static void write_last_file(const char *path) {
    FILE *f = fopen(last_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", path);
    fclose(f);
}

/* ── Snapshot ────────────────────────────────────────────────────────────── */
/*
 * Called whenever a file is successfully loaded or saved.
 * Captures the current devices[] state so Revert can restore it.
 */
static void take_snapshot(void) {
    n_snapshot = n_devices;
    memcpy(snapshot, devices, n_devices * sizeof(Device));
}

/*
 * Restores devices[] from the snapshot and re-applies all levels via wpctl.
 * wpctl_id values are preserved from the snapshot so no re-lookup is needed
 * for devices that were already resolved; newly unknown ones fall back to
 * the name+class search in get_wpctl_id().
 */
static void restore_snapshot(void) {
    n_devices = n_snapshot;
    memcpy(devices, snapshot, n_snapshot * sizeof(Device));
    apply_all_volumes();
}

/* ── Title / file tracking ───────────────────────────────────────────────── */

static void set_current_file(AppData *app, const char *path) {
    if (path && path[0]) {
        g_strlcpy(app->current_file, path, sizeof(app->current_file));
        char title[600];
        snprintf(title, sizeof(title), "PipeWire Volume Control  —  %s",
                 g_path_get_basename(path));
        gtk_window_set_title(GTK_WINDOW(app->window), title);
    } else {
        app->current_file[0] = '\0';
        gtk_window_set_title(GTK_WINDOW(app->window),
            "PipeWire Volume Control");
    }
}

/* ── Display name ────────────────────────────────────────────────────────── */

static void display_name(const char *full, char *out, int outlen) {
    if (g_str_has_prefix(full, "FDV_") || g_str_has_prefix(full, "FF_")) {
        g_strlcpy(out, full, outlen);
        return;
    }
    const char *dir_suffix = "";
    const char *body = full;
    if (g_str_has_prefix(full, "alsa_output.")) {
        dir_suffix = " (out)";
        body = full + strlen("alsa_output.");
    } else if (g_str_has_prefix(full, "alsa_input.")) {
        dir_suffix = " (in)";
        body = full + strlen("alsa_input.");
    }
    const char *dot = strchr(body, '.');
    int len = dot ? (int)(dot - body) : (int)strlen(body);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, body, len);
    out[len] = '\0';
    g_strlcat(out, dir_suffix, outlen);
}

/* ── Live read from wpctl ────────────────────────────────────────────────── */

static void load_live(void) {
    n_devices = 0;
    FILE *fp = popen("wpctl status", "r");
    if (!fp) { g_warning("Cannot run wpctl status"); return; }

    typedef struct { int id; double vol; } IdVol;
    IdVol candidates[256];
    int n_cand = 0;
    char line[512];

    while (fgets(line, sizeof(line), fp) && n_cand < 256) {
        if (!strstr(line, "[vol:")) continue;
        char clean[512]; int ci = 0;
        for (int i = 0; line[i] && ci < 510; i++) {
            unsigned char c = (unsigned char)line[i];
            if (c < 0x80 && (c >= 0x20 || c == '\t')) clean[ci++] = line[i];
        }
        clean[ci] = '\0';
        char *dot = strchr(clean, '.');
        if (!dot) continue;
        int idlen = dot - clean;
        if (idlen <= 0 || idlen >= 16) continue;
        char id_str[16] = {0};
        memcpy(id_str, clean, idlen); id_str[idlen] = '\0';
        char *s = id_str;
        while (*s == ' ' || *s == '*') s++;
        if (*s < '0' || *s > '9') continue;
        char *vstart = strstr(clean, "[vol:");
        if (!vstart) continue;
        candidates[n_cand].id  = atoi(s);
        candidates[n_cand].vol = atof(vstart + 5);
        n_cand++;
    }
    pclose(fp);

    for (int i = 0; i < n_cand && n_devices < MAX_DEVICES; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "wpctl inspect %d 2>/dev/null", candidates[i].id);
        FILE *fp2 = popen(cmd, "r");
        if (!fp2) continue;
        char node_name[MAX_NAME_LEN]   = {0};
        char node_class[MAX_CLASS_LEN] = {0};
        while (fgets(line, sizeof(line), fp2)) {
            if (strstr(line, "node.name") && !node_name[0]) {
                char *q1 = strchr(line, '"');
                if (q1) { char *q2 = strchr(q1+1,'"'); if (q2) { *q2='\0'; g_strlcpy(node_name,  q1+1, MAX_NAME_LEN);  } }
            }
            if (strstr(line, "media.class") && !node_class[0]) {
                char *q1 = strchr(line, '"');
                if (q1) { char *q2 = strchr(q1+1,'"'); if (q2) { *q2='\0'; g_strlcpy(node_class, q1+1, MAX_CLASS_LEN); } }
            }
        }
        pclose(fp2);
        if (!node_name[0] || !node_class[0]) continue;
        g_strlcpy(devices[n_devices].name,  node_name,        MAX_NAME_LEN);
        g_strlcpy(devices[n_devices].class, node_class,       MAX_CLASS_LEN);
        devices[n_devices].wpctl_id = candidates[i].id;
        devices[n_devices].vol      = candidates[i].vol;
        n_devices++;
    }
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

static int load_file(const char *path) {
    n_devices = 0;
    FILE *f = fopen(path, "r");
    if (!f) { g_warning("Cannot open: %s", path); return 0; }
    char line[512];
    while (fgets(line, sizeof(line), f) && n_devices < MAX_DEVICES) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char *p1 = strchr(line, '|');  if (!p1) continue;
        char *p2 = strchr(p1+1, '|'); if (!p2) continue;
        *p1 = '\0'; *p2 = '\0';
        g_strlcpy(devices[n_devices].name,  line,  MAX_NAME_LEN);
        g_strlcpy(devices[n_devices].class, p1+1,  MAX_CLASS_LEN);
        devices[n_devices].wpctl_id = -1;
        devices[n_devices].vol      = atof(p2+1);
        n_devices++;
    }
    fclose(f);
    return n_devices;
}

static void write_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { g_warning("Cannot write: %s", path); return; }
    for (int i = 0; i < n_devices; i++)
        fprintf(f, "%s|%s|%.2f\n", devices[i].name, devices[i].class, devices[i].vol);
    fclose(f);
}

static void save_temp(void) { write_file(temp_path); }

/* ── wpctl integration ───────────────────────────────────────────────────── */

static int get_wpctl_id(int idx) {
    if (idx < 0 || idx >= n_devices) return -1;
    if (devices[idx].wpctl_id >= 0) return devices[idx].wpctl_id;

    FILE *fp = popen("wpctl status", "r");
    if (!fp) return -1;
    int ids[256]; int n_ids = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && n_ids < 256) {
        if (!strstr(line, "[vol:")) continue;
        char clean[512]; int ci = 0;
        for (int i = 0; line[i] && ci < 510; i++) {
            unsigned char c = (unsigned char)line[i];
            if (c < 0x80 && (c >= 0x20 || c == '\t')) clean[ci++] = line[i];
        }
        clean[ci] = '\0';
        char *dot = strchr(clean, '.');
        if (!dot) continue;
        int len = dot - clean;
        if (len <= 0 || len >= 16) continue;
        char id_str[16] = {0};
        memcpy(id_str, clean, len); id_str[len] = '\0';
        char *s = id_str;
        while (*s == ' ' || *s == '*') s++;
        if (*s < '0' || *s > '9') continue;
        ids[n_ids++] = atoi(s);
    }
    pclose(fp);

    for (int i = 0; i < n_ids; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "wpctl inspect %d 2>/dev/null", ids[i]);
        FILE *fp2 = popen(cmd, "r");
        if (!fp2) continue;
        char nn[MAX_NAME_LEN]={0}, nc[MAX_CLASS_LEN]={0};
        while (fgets(line, sizeof(line), fp2)) {
            if (strstr(line,"node.name") && !nn[0]) {
                char *q1=strchr(line,'"'); if(q1){char *q2=strchr(q1+1,'"'); if(q2){*q2='\0'; g_strlcpy(nn,q1+1,MAX_NAME_LEN);}}
            }
            if (strstr(line,"media.class") && !nc[0]) {
                char *q1=strchr(line,'"'); if(q1){char *q2=strchr(q1+1,'"'); if(q2){*q2='\0'; g_strlcpy(nc,q1+1,MAX_CLASS_LEN);}}
            }
        }
        pclose(fp2);
        if (strcmp(nn, devices[idx].name)==0 && strcmp(nc, devices[idx].class)==0) {
            devices[idx].wpctl_id = ids[i];
            return ids[i];
        }
    }
    return -1;
}

static void apply_volume(int idx) {
    if (idx < 0 || idx >= n_devices) return;
    int id = get_wpctl_id(idx);
    if (id < 0) { g_warning("wpctl ID not found for %s", devices[idx].name); return; }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume %d %.2f", id, devices[idx].vol);
    system(cmd);
}

static void apply_all_volumes(void) {
    for (int i = 0; i < n_devices; i++) apply_volume(i);
}

/* ── GUI helpers ─────────────────────────────────────────────────────────── */

/*
 * Revert is only enabled when a file is loaded and there are unsaved changes.
 * Save is always available.
 */
static void set_unsaved(AppData *app, gboolean unsaved) {
    app->unsaved = unsaved;
    /* Revert only makes sense if there is a file snapshot to go back to */
    gtk_widget_set_sensitive(app->revert_btn,
        unsaved && n_snapshot > 0);
    gtk_label_set_text(GTK_LABEL(app->status_lbl),
        unsaved ? "⚠  Unsaved changes" : "");
}

static void populate_list(AppData *app) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->list_box)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(app->list_box), child);

    for (int i = 0; i < n_devices; i++) {
        if (!app->show_all && devices[i].vol >= 0.995) continue;
        char dname[MAX_NAME_LEN];
        display_name(devices[i].name, dname, sizeof(dname));
        char label[256];
        snprintf(label, sizeof(label), "%s  [%d%%]",
                 dname, (int)round(devices[i].vol * 100.0));
        GtkWidget *row_lbl = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(row_lbl), 0.0f);
        gtk_widget_set_margin_start(row_lbl, 8);
        gtk_widget_set_margin_end(row_lbl,   8);
        gtk_widget_set_margin_top(row_lbl,   4);
        gtk_widget_set_margin_bottom(row_lbl,4);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_lbl);
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
        set_step_buttons_sensitive(app, FALSE);
        return;
    }
    Device *d = &devices[idx];
    char dname[MAX_NAME_LEN];
    display_name(d->name, dname, sizeof(dname));
    gtk_label_set_text(GTK_LABEL(app->lbl_name),  dname);
    gtk_label_set_text(GTK_LABEL(app->lbl_class), d->class);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)round(d->vol * 100.0));
    gtk_label_set_text(GTK_LABEL(app->lbl_pct), pct);

    set_step_buttons_sensitive(app, TRUE);
}

/* ── Signal callbacks ────────────────────────────────────────────────────── */

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer user_data) {
    (void)lb;
    AppData *app = (AppData *)user_data;
    if (!row) { update_right_panel(app, -1); return; }
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "device-idx"));
    update_right_panel(app, idx);
}

/* ── Step button helpers ─────────────────────────────────────────────────── */

static void set_step_buttons_sensitive(AppData *app, gboolean sensitive) {
    gtk_widget_set_sensitive(app->btn_dec5, sensitive);
    gtk_widget_set_sensitive(app->btn_dec1, sensitive);
    gtk_widget_set_sensitive(app->btn_inc1, sensitive);
    gtk_widget_set_sensitive(app->btn_inc5, sensitive);
}

static void adjust_volume(AppData *app, double delta) {
    if (app->selected_idx < 0) return;
    double val = devices[app->selected_idx].vol + delta;
    if (val < 0.0) val = 0.0;
    if (val > 1.0) val = 1.0;
    devices[app->selected_idx].vol = val;

    /* Update percentage label */
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)round(val * 100.0));
    gtk_label_set_text(GTK_LABEL(app->lbl_pct), pct);

    /* Update list row label */
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->list_box));
    if (row) {
        GtkWidget *lbl = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        char dname[MAX_NAME_LEN];
        display_name(devices[app->selected_idx].name, dname, sizeof(dname));
        char label[256];
        snprintf(label, sizeof(label), "%s  [%d%%]", dname, (int)round(val * 100.0));
        gtk_label_set_text(GTK_LABEL(lbl), label);
    }

    apply_volume(app->selected_idx);
    save_temp();
    set_unsaved(app, TRUE);
}

static void on_dec5(GtkButton *btn, gpointer user_data) { (void)btn; adjust_volume((AppData *)user_data, -0.05); }
static void on_dec1(GtkButton *btn, gpointer user_data) { (void)btn; adjust_volume((AppData *)user_data, -0.01); }
static void on_inc1(GtkButton *btn, gpointer user_data) { (void)btn; adjust_volume((AppData *)user_data,  0.01); }
static void on_inc5(GtkButton *btn, gpointer user_data) { (void)btn; adjust_volume((AppData *)user_data,  0.05); }

static void on_toggle_show_all(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    app->show_all = !app->show_all;
    gtk_button_set_label(GTK_BUTTON(app->toggle_btn),
        app->show_all ? "Show active only" : "Show all devices");
    populate_list(app);
    update_right_panel(app, -1);
}

static void on_refresh_live(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    load_live();
    n_snapshot = 0;   /* no file loaded — Revert unavailable */
    save_temp();
    populate_list(app);
    update_right_panel(app, -1);
    set_unsaved(app, FALSE);
    set_current_file(app, NULL);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), "Refreshed from live.");
}

/* Revert: restore snapshot from last Load or Save */
static void on_revert(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    int prev_idx = app->selected_idx;
    restore_snapshot();
    save_temp();
    populate_list(app);
    /* Re-select the previously selected device if it is still in the list */
    gboolean reselected = FALSE;
    if (prev_idx >= 0 && prev_idx < n_devices) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(app->list_box), 0);
        while (row) {
            int idx = GPOINTER_TO_INT(
                g_object_get_data(G_OBJECT(row), "device-idx"));
            if (idx == prev_idx) {
                gtk_list_box_select_row(GTK_LIST_BOX(app->list_box), row);
                update_right_panel(app, prev_idx);
                reselected = TRUE;
                break;
            }
            row = GTK_LIST_BOX_ROW(gtk_widget_get_next_sibling(GTK_WIDGET(row)));
        }
    }
    if (!reselected) update_right_panel(app, -1);
    set_unsaved(app, FALSE);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), "Reverted to saved levels.");
}

/* Save */
static void on_save_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;
    write_file(path);
    write_last_file(path);
    take_snapshot();   /* saved state is now the new revert point */
    set_current_file(app, path);
    set_unsaved(app, FALSE);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), "Saved.");
    g_free(path);
}

static void on_save(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Save levels...");
    if (app->current_file[0]) {
        GFile *cur = g_file_new_for_path(app->current_file);
        gtk_file_dialog_set_initial_file(dlg, cur);
        g_object_unref(cur);
    } else {
        gtk_file_dialog_set_initial_name(dlg, "levels.db");
        GFile *dir = g_file_new_for_path(default_dir);
        gtk_file_dialog_set_initial_folder(dlg, dir);
        g_object_unref(dir);
    }
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Level files (*.db)");
    gtk_file_filter_add_pattern(filter, "*.db");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);
    gtk_file_dialog_save(dlg, GTK_WINDOW(app->window), NULL, on_save_response, app);
    g_object_unref(dlg);
}

/* Load */
static void on_load_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;
    if (!load_file(path)) {
        gtk_label_set_text(GTK_LABEL(app->status_lbl), "⚠  File empty or unreadable.");
        g_free(path);
        return;
    }
    apply_all_volumes();
    save_temp();
    take_snapshot();   /* loaded state becomes the revert point */
    populate_list(app);
    update_right_panel(app, -1);
    write_last_file(path);
    set_current_file(app, path);
    set_unsaved(app, FALSE);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), "Loaded.");
    g_free(path);
}

static void on_load(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Load levels...");
    GFile *dir = g_file_new_for_path(default_dir);
    gtk_file_dialog_set_initial_folder(dlg, dir);
    g_object_unref(dir);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Level files (*.db)");
    gtk_file_filter_add_pattern(filter, "*.db");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);
    gtk_file_dialog_open(dlg, GTK_WINDOW(app->window), NULL, on_load_response, app);
    g_object_unref(dlg);
}

/* Close alert */
static void on_close_alert_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
    if (button == 1) {
        app->unsaved = FALSE;
        gtk_window_destroy(GTK_WINDOW(app->window));
    }
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
    (void)window;
    AppData *app = (AppData *)user_data;
    if (!app->unsaved) return FALSE;
    GtkAlertDialog *alert = gtk_alert_dialog_new("Unsaved level changes");
    gtk_alert_dialog_set_detail(alert,
        "Live levels will remain as adjusted.\n"
        "No file has been saved.");
    const char *buttons[] = { "Cancel", "Close anyway", NULL };
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 0);
    gtk_alert_dialog_choose(alert, GTK_WINDOW(app->window), NULL,
        on_close_alert_response, app);
    g_object_unref(alert);
    return TRUE;
}

/* ── CSS ─────────────────────────────────────────────────────────────────── */

static const char *APP_CSS =
    "window { background-color: #2b2b2b; }"

    /* Device name — bold white */
    ".device-name  { font-size: 16px; font-weight: bold; color: #ffffff; }"

    /* Class — larger and clearly visible */
    ".device-class { font-size: 13px; color: #b0bec5; }"

    /* Big percentage readout */
    ".pct-label    { font-size: 32px; font-weight: bold; color: #4fc3f7; }"

    /* Status bar */
    ".status-label { font-size: 11px; color: #ffb74d; }"

    /* Device list */
    "list          { background-color: #2b2b2b; }"
    "row           { color: #cccccc; font-size: 12px; }"
    "row:selected  { background-color: #1565c0; color: #ffffff; }"

    /* Slider */
    "scale trough  { background-color: #616161; min-height: 8px; }"
    "scale highlight { background-color: #4fc3f7; }"

    /* Standard buttons — light grey, clearly readable */
    "button {"
    "  background: #607d8b;"    /* blue-grey */
    "  color: #ffffff;"
    "  border: none; border-radius: 4px; padding: 6px 14px; }"
    "button:hover   { background: #78909c; }"
    "button:disabled { background: #37474f; color: #78909c; }"

    /* Save — green */
    ".btn-save {"
    "  background: #2e7d32; color: #ffffff;"
    "  border: none; border-radius: 4px; padding: 6px 16px; }"
    ".btn-save:hover    { background: #388e3c; }"
    ".btn-save:disabled { background: #1b3a1d; color: #4a7a4d; }"

    /* Revert — amber, distinct from destructive red */
    ".btn-revert {"
    "  background: #e65100; color: #ffffff;"
    "  border: none; border-radius: 4px; padding: 6px 16px; }"
    ".btn-revert:hover    { background: #ef6c00; }"
    ".btn-revert:disabled { background: #3e1f00; color: #7a4010; }"

    /* Step buttons — compact */
    ".btn-step {"
    "  background: #546e7a; color: #ffffff;"
    "  border: none; border-radius: 4px;"
    "  padding: 3px 10px;"
    "  font-size: 12px; font-weight: bold; }"
    ".btn-step:hover    { background: #607d8b; }"
    ".btn-step:disabled { background: #2e3d44; color: #546e7a; }"


    "separator { background-color: #444444; min-height: 1px; margin: 2px 0; }";

/* ── App activate ────────────────────────────────────────────────────────── */

static void activate(GtkApplication *gtk_app, gpointer user_data) {
    (void)user_data;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, APP_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    AppData *app = g_new0(AppData, 1);
    app->show_all        = FALSE;
    app->unsaved         = FALSE;
    app->selected_idx    = -1;
    app->current_file[0] = '\0';

    build_paths();

    char *last = read_last_file();
    if (last && load_file(last)) {
        apply_all_volumes();
        save_temp();
        take_snapshot();
    } else {
        load_live();
        save_temp();
        n_snapshot = 0;
        if (last) { g_free(last); last = NULL; }
    }

    /* ── Window ── */
    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "PipeWire Volume Control");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 400, 320);
    gtk_window_set_resizable(GTK_WINDOW(app->window), FALSE);
    g_signal_connect(app->window, "close-request",
        G_CALLBACK(on_close_request), app);

    if (last) { set_current_file(app, last); g_free(last); }

    /* ── Root vbox ── */
    GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), root_vbox);

    /* ── Main hbox ── */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_append(GTK_BOX(root_vbox), hbox);

    /* ══ LEFT PANEL ══ */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(left_vbox, 220, -1);
    gtk_widget_set_margin_start(left_vbox,  8);
    gtk_widget_set_margin_end(left_vbox,    8);
    gtk_widget_set_margin_top(left_vbox,   10);
    gtk_widget_set_margin_bottom(left_vbox, 8);
    gtk_box_append(GTK_BOX(hbox), left_vbox);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(left_vbox), scroll);

    app->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->list_box), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->list_box);
    g_signal_connect(app->list_box, "row-selected",
        G_CALLBACK(on_row_selected), app);


    /* ── Vertical divider ── */
    gtk_box_append(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* ══ RIGHT PANEL ══ */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(right_vbox, TRUE);
    gtk_box_append(GTK_BOX(hbox), right_vbox);

    GtkWidget *spacer_top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer_top, TRUE);
    gtk_box_append(GTK_BOX(right_vbox), spacer_top);

    GtkWidget *ctrl_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(ctrl_vbox, 12);
    gtk_widget_set_margin_end(ctrl_vbox,   12);
    gtk_box_append(GTK_BOX(right_vbox), ctrl_vbox);

    app->lbl_name = gtk_label_new("No device selected");
    gtk_widget_add_css_class(app->lbl_name, "device-name");
    gtk_label_set_wrap(GTK_LABEL(app->lbl_name), TRUE);
    gtk_label_set_xalign(GTK_LABEL(app->lbl_name), 0.0f);
    gtk_box_append(GTK_BOX(ctrl_vbox), app->lbl_name);

    app->lbl_class = gtk_label_new("");
    gtk_widget_add_css_class(app->lbl_class, "device-class");
    gtk_label_set_xalign(GTK_LABEL(app->lbl_class), 0.0f);
    gtk_box_append(GTK_BOX(ctrl_vbox), app->lbl_class);

    app->lbl_pct = gtk_label_new("");
    gtk_widget_add_css_class(app->lbl_pct, "pct-label");
    gtk_label_set_xalign(GTK_LABEL(app->lbl_pct), 0.0f);
    gtk_box_append(GTK_BOX(ctrl_vbox), app->lbl_pct);

    /* ── Step buttons: --  -  +  ++ ── */
    GtkWidget *step_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(step_hbox, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(ctrl_vbox), step_hbox);

    app->btn_dec5 = gtk_button_new_with_label("− −");
    gtk_widget_add_css_class(app->btn_dec5, "btn-step");
    gtk_widget_set_sensitive(app->btn_dec5, FALSE);
    gtk_widget_set_tooltip_text(app->btn_dec5, "Decrease 5%");
    gtk_box_append(GTK_BOX(step_hbox), app->btn_dec5);
    g_signal_connect(app->btn_dec5, "clicked", G_CALLBACK(on_dec5), app);

    app->btn_dec1 = gtk_button_new_with_label("−");
    gtk_widget_add_css_class(app->btn_dec1, "btn-step");
    gtk_widget_set_sensitive(app->btn_dec1, FALSE);
    gtk_widget_set_tooltip_text(app->btn_dec1, "Decrease 1%");
    gtk_box_append(GTK_BOX(step_hbox), app->btn_dec1);
    g_signal_connect(app->btn_dec1, "clicked", G_CALLBACK(on_dec1), app);

    app->btn_inc1 = gtk_button_new_with_label("+");
    gtk_widget_add_css_class(app->btn_inc1, "btn-step");
    gtk_widget_set_sensitive(app->btn_inc1, FALSE);
    gtk_widget_set_tooltip_text(app->btn_inc1, "Increase 1%");
    gtk_box_append(GTK_BOX(step_hbox), app->btn_inc1);
    g_signal_connect(app->btn_inc1, "clicked", G_CALLBACK(on_inc1), app);

    app->btn_inc5 = gtk_button_new_with_label("+ +");
    gtk_widget_add_css_class(app->btn_inc5, "btn-step");
    gtk_widget_set_sensitive(app->btn_inc5, FALSE);
    gtk_widget_set_tooltip_text(app->btn_inc5, "Increase 5%");
    gtk_box_append(GTK_BOX(step_hbox), app->btn_inc5);
    g_signal_connect(app->btn_inc5, "clicked", G_CALLBACK(on_inc5), app);

    /* Action buttons */
    GtkWidget *action_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(action_hbox, 8);
    gtk_box_append(GTK_BOX(ctrl_vbox), action_hbox);

    GtkWidget *save_btn = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save_btn, "btn-save");
    gtk_box_append(GTK_BOX(action_hbox), save_btn);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save), app);

    app->revert_btn = gtk_button_new_with_label("Revert");
    gtk_widget_add_css_class(app->revert_btn, "btn-revert");
    gtk_widget_set_sensitive(app->revert_btn, FALSE);
    gtk_box_append(GTK_BOX(action_hbox), app->revert_btn);
    g_signal_connect(app->revert_btn, "clicked", G_CALLBACK(on_revert), app);

    GtkWidget *load_btn = gtk_button_new_with_label("Load");
    gtk_widget_add_css_class(load_btn, "btn-save");
    gtk_box_append(GTK_BOX(action_hbox), load_btn);
    g_signal_connect(load_btn, "clicked", G_CALLBACK(on_load), app);

    /* Utility buttons row */
    GtkWidget *util_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(ctrl_vbox), util_hbox);

    app->toggle_btn = gtk_button_new_with_label("Show all devices");
    gtk_box_append(GTK_BOX(util_hbox), app->toggle_btn);
    g_signal_connect(app->toggle_btn, "clicked",
        G_CALLBACK(on_toggle_show_all), app);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh live");
    gtk_box_append(GTK_BOX(util_hbox), refresh_btn);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_live), app);

    GtkWidget *spacer_bot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer_bot, TRUE);
    gtk_box_append(GTK_BOX(right_vbox), spacer_bot);

    /* ── Status bar ── */
    gtk_box_append(GTK_BOX(root_vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    app->status_lbl = gtk_label_new("");
    gtk_widget_add_css_class(app->status_lbl, "status-label");
    gtk_label_set_xalign(GTK_LABEL(app->status_lbl), 1.0f);
    gtk_widget_set_margin_end(app->status_lbl,    10);
    gtk_widget_set_margin_top(app->status_lbl,     3);
    gtk_widget_set_margin_bottom(app->status_lbl,  3);
    gtk_box_append(GTK_BOX(root_vbox), app->status_lbl);

    populate_list(app);
    gtk_window_present(GTK_WINDOW(app->window));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new(
        "uk.radio.pw-volctl",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
