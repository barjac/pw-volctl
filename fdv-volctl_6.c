/*
 * fdv-volctl.c
 * FreeDV Volume Control - GTK4/C
 *
 * On startup reads live volumes directly from wpctl - no db dependency.
 * Adjustments are applied via wpctl and written to a temp file.
 * "Load from db" / "Save to db" give explicit control over the db file.
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
#define TEMP_PATH      "%s/.config/pipewire/saved-volumes.tmp"

/* ── Data model ─────────────────────────────────────────────────────────── */

typedef struct {
    char   name[MAX_NAME_LEN];    /* full node name, e.g. alsa_output.usb-headset.analog-stereo */
    char   class[MAX_CLASS_LEN];  /* e.g. Audio/Sink */
    int    wpctl_id;              /* cached live ID; -1 if unknown */
    double vol;                   /* 0.0 – 1.0 */
} Device;

static Device  devices[MAX_DEVICES];
static int     n_devices = 0;
static char    db_path[256];
static char    temp_path[256];

/* ── Application state ───────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *window;
    GtkWidget *list_box;
    GtkWidget *lbl_name;
    GtkWidget *lbl_class;
    GtkWidget *lbl_pct;
    GtkWidget *slider;
    GtkWidget *toggle_btn;
    GtkWidget *save_btn;
    GtkWidget *revert_btn;
    GtkWidget *status_lbl;
    gboolean   show_all;
    gboolean   unsaved;
    int        selected_idx;
} AppData;

/* ── Forward declarations ──────────────────────────────────────────────────────────────────────────────── */
static int   load_file(const char *path);
static int   load_db_file(void);
static void  write_file(const char *path);
static void  save_temp(void);
static void  save_to_db(void);
static void  apply_all_volumes(void);
static void  populate_list(AppData *app);
static void  update_right_panel(AppData *app, int idx);
static void  set_unsaved(AppData *app, gboolean unsaved);

/* ── Path helpers ────────────────────────────────────────────────────────── */

static void build_paths(void) {
    const char *home = g_get_home_dir();
    snprintf(db_path,   sizeof(db_path),   DB_PATH,   home);
    snprintf(temp_path, sizeof(temp_path), TEMP_PATH, home);
}

/* ── Display name ────────────────────────────────────────────────────────── */
/*
 * Produce an unambiguous short label from a full node name.
 *
 * alsa_output.usb-headset.analog-stereo  →  "usb-headset (out)"
 * alsa_input.usb-headset.mono-fallback   →  "usb-headset (in)"
 * alsa_output.pci-0000_00_1b.0.analog-stereo → "pci-0000_00_1b (out)"
 * FDV_RX_in                              →  "FDV_RX_in"
 * FF_out                                 →  "FF_out"
 */
static void display_name(const char *full, char *out, int outlen) {
    /* FDV_ and FF_ nodes: show as-is */
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

    /* Take the first dot-delimited segment of body as the device identifier */
    const char *dot = strchr(body, '.');
    int len = dot ? (int)(dot - body) : (int)strlen(body);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, body, len);
    out[len] = '\0';

    /* Append direction suffix if it fits */
    g_strlcat(out, dir_suffix, outlen);
}

/* ── Live read from wpctl ────────────────────────────────────────────────── */
/*
 * Populates devices[] directly from "wpctl status" + "wpctl inspect".
 * The volume is taken straight from the [vol: N.NN] field on each status line.
 * wpctl IDs are cached in device.wpctl_id for immediate use.
 */
static void load_live(void) {
    n_devices = 0;

    FILE *fp = popen("wpctl status", "r");
    if (!fp) { g_warning("Cannot run wpctl status"); return; }

    /* Collect (id, vol) pairs from lines that have [vol:] */
    typedef struct { int id; double vol; } IdVol;
    IdVol candidates[256];
    int n_cand = 0;
    char line[512];

    while (fgets(line, sizeof(line), fp) && n_cand < 256) {
        if (!strstr(line, "[vol:")) continue;

        /* Strip box-drawing / non-ASCII so we see plain ASCII */
        char clean[512];
        int ci = 0;
        for (int i = 0; line[i] && ci < 510; i++) {
            unsigned char c = (unsigned char)line[i];
            if (c < 0x80 && (c >= 0x20 || c == '\t')) clean[ci++] = line[i];
        }
        clean[ci] = '\0';

        /* ID: digits before the first '.' */
        char *dot = strchr(clean, '.');
        if (!dot) continue;
        int idlen = dot - clean;
        if (idlen <= 0 || idlen >= 16) continue;
        char id_str[16] = {0};
        memcpy(id_str, clean, idlen);
        id_str[idlen] = '\0';
        char *s = id_str;
        while (*s == ' ' || *s == '*') s++;
        if (*s < '0' || *s > '9') continue;
        int id = atoi(s);

        /* Vol: inside [vol: N.NN] */
        char *vstart = strstr(clean, "[vol:");
        if (!vstart) continue;
        double vol = atof(vstart + 5);

        candidates[n_cand].id  = id;
        candidates[n_cand].vol = vol;
        n_cand++;
    }
    pclose(fp);

    /* Inspect each candidate for name and class */
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

        /* Skip nodes without a class (monitors, virtual internals, etc.) */
        if (!node_name[0] || !node_class[0]) continue;

        g_strlcpy(devices[n_devices].name,  node_name,          MAX_NAME_LEN);
        g_strlcpy(devices[n_devices].class, node_class,         MAX_CLASS_LEN);
        devices[n_devices].wpctl_id = candidates[i].id;
        devices[n_devices].vol      = candidates[i].vol;
        n_devices++;
    }
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

/* Load from arbitrary path — used by "Load from file..." */
static int load_file(const char *path) {
    n_devices = 0;
    FILE *f = fopen(path, "r");
    if (!f) { g_warning("Cannot open: %s", path); return 0; }
    char line[512];
    while (fgets(line, sizeof(line), f) && n_devices < MAX_DEVICES) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        char *p2 = strchr(p1+1, '|');
        if (!p2) continue;
        *p1 = '\0'; *p2 = '\0';
        g_strlcpy(devices[n_devices].name,  line,    MAX_NAME_LEN);
        g_strlcpy(devices[n_devices].class, p1+1,    MAX_CLASS_LEN);
        devices[n_devices].wpctl_id = -1;
        devices[n_devices].vol      = atof(p2+1);
        n_devices++;
    }
    fclose(f);
    return n_devices;
}

static int load_db_file(void) {
    n_devices = 0;
    FILE *f = fopen(db_path, "r");
    if (!f) { g_warning("Cannot open db: %s", db_path); return 0; }
    char line[512];
    while (fgets(line, sizeof(line), f) && n_devices < MAX_DEVICES) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        char *p2 = strchr(p1+1, '|');
        if (!p2) continue;
        *p1 = '\0'; *p2 = '\0';
        g_strlcpy(devices[n_devices].name,  line,    MAX_NAME_LEN);
        g_strlcpy(devices[n_devices].class, p1+1,    MAX_CLASS_LEN);
        devices[n_devices].wpctl_id = -1;   /* will be looked up on demand */
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
        fprintf(f, "%s|%s|%.2f\n",
                devices[i].name, devices[i].class, devices[i].vol);
    fclose(f);
}

static void save_temp(void)    { write_file(temp_path); }
static void save_to_db(void)   { write_file(db_path); write_file(temp_path); }

/* ── wpctl apply ─────────────────────────────────────────────────────────── */

/* Find (or refresh) the wpctl ID for a device */
static int get_wpctl_id(int idx) {
    if (idx < 0 || idx >= n_devices) return -1;
    if (devices[idx].wpctl_id >= 0) return devices[idx].wpctl_id;

    /* Fallback: search by name+class (used after db load) */
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
        char nname[MAX_NAME_LEN]={0}, nclass[MAX_CLASS_LEN]={0};
        while (fgets(line, sizeof(line), fp2)) {
            if (strstr(line,"node.name") && !nname[0]) {
                char *q1=strchr(line,'"'); if (q1){char *q2=strchr(q1+1,'"'); if(q2){*q2='\0'; g_strlcpy(nname,q1+1,MAX_NAME_LEN);}}
            }
            if (strstr(line,"media.class") && !nclass[0]) {
                char *q1=strchr(line,'"'); if (q1){char *q2=strchr(q1+1,'"'); if(q2){*q2='\0'; g_strlcpy(nclass,q1+1,MAX_CLASS_LEN);}}
            }
        }
        pclose(fp2);
        if (strcmp(nname, devices[idx].name)==0 && strcmp(nclass, devices[idx].class)==0) {
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

static void set_unsaved(AppData *app, gboolean unsaved) {
    app->unsaved = unsaved;
    gtk_widget_set_sensitive(app->save_btn,   unsaved);
    gtk_widget_set_sensitive(app->revert_btn, unsaved);
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
        int pct = (int)round(devices[i].vol * 100.0);
        char label[256];
        snprintf(label, sizeof(label), "%s  [%d%%]", dname, pct);

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
        gtk_widget_set_sensitive(app->slider, FALSE);
        return;
    }
    Device *d = &devices[idx];
    char dname[MAX_NAME_LEN];
    display_name(d->name, dname, sizeof(dname));
    gtk_label_set_text(GTK_LABEL(app->lbl_name),  dname);
    gtk_label_set_text(GTK_LABEL(app->lbl_class), d->class);
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", (int)round(d->vol * 100.0));
    gtk_label_set_text(GTK_LABEL(app->lbl_pct), pct_str);

    g_signal_handlers_block_matched(app->slider,
        G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, app);
    gtk_range_set_value(GTK_RANGE(app->slider), d->vol * 100.0);
    g_signal_handlers_unblock_matched(app->slider,
        G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, app);

    gtk_widget_set_sensitive(app->slider, TRUE);
}

/* ── Signal callbacks ────────────────────────────────────────────────────── */

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer user_data) {
    (void)lb;
    AppData *app = (AppData *)user_data;
    if (!row) { update_right_panel(app, -1); return; }
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "device-idx"));
    update_right_panel(app, idx);
}

static void on_slider_value_changed(GtkRange *range, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (app->selected_idx < 0) return;
    double val = gtk_range_get_value(range) / 100.0;
    devices[app->selected_idx].vol = val;

    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", (int)round(val * 100.0));
    gtk_label_set_text(GTK_LABEL(app->lbl_pct), pct_str);

    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->list_box));
    if (row) {
        GtkWidget *lbl = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        char dname[MAX_NAME_LEN];
        display_name(devices[app->selected_idx].name, dname, sizeof(dname));
        char label[256];
        snprintf(label, sizeof(label), "%s  [%d%%]", dname, (int)round(val*100.0));
        gtk_label_set_text(GTK_LABEL(lbl), label);
    }
}

/* On mouse release: apply via wpctl, write to temp, mark unsaved */
static gboolean on_slider_released(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (gdk_event_get_event_type(event) != GDK_BUTTON_RELEASE) return FALSE;
    apply_volume(app->selected_idx);
    save_temp();
    set_unsaved(app, TRUE);
    return FALSE;
}

static gboolean on_slider_key_released(GtkEventController *ctrl, guint keyval,
                                        guint keycode, GdkModifierType state,
                                        gpointer user_data) {
    (void)ctrl; (void)keycode; (void)state;
    AppData *app = (AppData *)user_data;
    if (keyval == GDK_KEY_Left  || keyval == GDK_KEY_Right ||
        keyval == GDK_KEY_Up    || keyval == GDK_KEY_Down  ||
        keyval == GDK_KEY_Home  || keyval == GDK_KEY_End) {
        apply_volume(app->selected_idx);
        save_temp();
        set_unsaved(app, TRUE);
    }
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

/* Refresh: re-read live levels from wpctl, no db involvement */
static void on_refresh_live(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    load_live();
    save_temp();
    populate_list(app);
    update_right_panel(app, -1);
    set_unsaved(app, FALSE);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), "Refreshed from live.");
}

/* Load from db: read db, apply all levels via wpctl */
static void on_load_db(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    if (load_db_file()) {
        apply_all_volumes();
        save_temp();
        populate_list(app);
        update_right_panel(app, -1);
        set_unsaved(app, FALSE);
        gtk_label_set_text(GTK_LABEL(app->status_lbl), "Loaded from db.");
    } else {
        gtk_label_set_text(GTK_LABEL(app->status_lbl), "⚠  db file not found.");
    }
}

/* Save to db */
static void on_save(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    save_to_db();
    set_unsaved(app, FALSE);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), "Saved to db.");
}

/* Save As: file chooser -> write current state to chosen file */
static void on_save_as_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;   /* user cancelled */
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;
    write_file(path);
    char msg[320];
    snprintf(msg, sizeof(msg), "Saved as: %s", path);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), msg);
    g_free(path);
}

static void on_save_as(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Save levels as...");
    gtk_file_dialog_set_initial_name(dlg, "levels.db");

    GFile *init_dir = g_file_new_for_path(g_path_get_dirname(db_path));
    gtk_file_dialog_set_initial_folder(dlg, init_dir);
    g_object_unref(init_dir);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Level db files (*.db)");
    gtk_file_filter_add_pattern(filter, "*.db");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);

    gtk_file_dialog_save(dlg, GTK_WINDOW(app->window), NULL,
        on_save_as_response, app);
    g_object_unref(dlg);
}

/* Load From: file chooser -> load and apply levels from chosen file */
static void on_load_from_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;   /* user cancelled */
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;

    int prev = n_devices;
    load_file(path);
    if (n_devices == 0) {
        n_devices = prev;   /* restore if file was empty/invalid */
        gtk_label_set_text(GTK_LABEL(app->status_lbl), "⚠  File empty or unreadable.");
        g_free(path);
        return;
    }
    /* IDs not known from file — clear them so get_wpctl_id() will search */
    for (int i = 0; i < n_devices; i++) devices[i].wpctl_id = -1;
    apply_all_volumes();
    save_temp();
    populate_list(app);
    update_right_panel(app, -1);
    set_unsaved(app, TRUE);   /* loaded file differs from default db until saved */

    char msg[320];
    snprintf(msg, sizeof(msg), "Loaded: %s", path);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), msg);
    g_free(path);
}

static void on_load_from(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Load levels from...");

    GFile *init_dir = g_file_new_for_path(g_path_get_dirname(db_path));
    gtk_file_dialog_set_initial_folder(dlg, init_dir);
    g_object_unref(init_dir);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Level db files (*.db)");
    gtk_file_filter_add_pattern(filter, "*.db");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);

    gtk_file_dialog_open(dlg, GTK_WINDOW(app->window), NULL,
        on_load_from_response, app);
    g_object_unref(dlg);
}

/* Revert: reload live levels from wpctl, discarding adjustments */
static void on_revert(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppData *app = (AppData *)user_data;
    load_live();
    save_temp();
    populate_list(app);
    update_right_panel(app, -1);
    set_unsaved(app, FALSE);
    gtk_label_set_text(GTK_LABEL(app->status_lbl), "Reverted to live levels.");
}

/* Async callback for the GtkAlertDialog on close */
static void on_close_alert_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
    /* Button indices match gtk_alert_dialog_set_buttons array:
     *  0 = Cancel
     *  1 = Close anyway
     *  2 = Save and close  */
    switch (button) {
        case 2:                     /* Save and close */
            save_to_db();
            /* fall through */
        case 1:                     /* Close anyway */
            app->unsaved = FALSE;   /* prevent on_close_request re-intercepting */
            gtk_window_destroy(GTK_WINDOW(app->window));
            break;
        default:                    /* Cancel or dismissed */
            break;
    }
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
    (void)window;
    AppData *app = (AppData *)user_data;
    if (!app->unsaved) return FALSE;   /* allow close */

    GtkAlertDialog *alert = gtk_alert_dialog_new(
        "You have unsaved volume changes.");
    gtk_alert_dialog_set_detail(alert,
        "Live levels will remain as adjusted.\n"
        "The saved db will be unchanged if you close without saving.");

    const char *buttons[] = { "Cancel", "Close anyway", "Save and close", NULL };
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 0);

    gtk_alert_dialog_choose(alert,
        GTK_WINDOW(app->window),
        NULL,
        on_close_alert_response,
        app);
    g_object_unref(alert);
    return TRUE;   /* intercept until user responds */
}

/* ── CSS ─────────────────────────────────────────────────────────────────── */

static const char *APP_CSS =
    "window { background-color: #2b2b2b; }"
    ".device-name { font-size: 15px; font-weight: bold; color: #e0e0e0; }"
    ".device-class { font-size: 11px; color: #888888; }"
    ".pct-label { font-size: 24px; font-weight: bold; color: #4fc3f7; }"
    ".status-label { font-size: 11px; color: #ffb74d; }"
    "list { background-color: #2b2b2b; }"
    "row { color: #cccccc; font-size: 12px; }"
    "row:selected { background-color: #1565c0; color: #ffffff; }"
    "scale trough { background-color: #555555; min-height: 8px; }"
    "scale highlight { background-color: #4fc3f7; }"
    ".btn-save {"
    "  background: #1b5e20; color: #e0e0e0;"
    "  border: none; border-radius: 4px; padding: 6px 12px; }"
    ".btn-save:hover { background: #2e7d32; }"
    ".btn-save:disabled { background: #333333; color: #555555; }"
    ".btn-revert {"
    "  background: #b71c1c; color: #e0e0e0;"
    "  border: none; border-radius: 4px; padding: 6px 12px; }"
    ".btn-revert:hover { background: #c62828; }"
    ".btn-revert:disabled { background: #333333; color: #555555; }"
    "button { background: #444444; color: #e0e0e0;"
    "  border: none; border-radius: 4px; padding: 6px 12px; }"
    "button:hover { background: #555555; }";

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
    app->show_all     = FALSE;
    app->unsaved      = FALSE;
    app->selected_idx = -1;

    build_paths();
    load_live();       /* read directly from wpctl - no db dependency */
    save_temp();       /* seed the temp file with current live state */

    /* ── Window ── */
    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "PipeWire Volume Control");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 560, 340);
    gtk_window_set_resizable(GTK_WINDOW(app->window), FALSE);
    g_signal_connect(app->window, "close-request",
        G_CALLBACK(on_close_request), app);

    /* ── Root vbox ── */
    GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), root_vbox);

    /* ── Main hbox ── */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 10);
    gtk_widget_set_margin_end(hbox, 10);
    gtk_widget_set_margin_top(hbox, 10);
    gtk_widget_set_margin_bottom(hbox, 6);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_append(GTK_BOX(root_vbox), hbox);

    /* ── Left panel ── */
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
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->list_box), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->list_box);
    g_signal_connect(app->list_box, "row-selected",
        G_CALLBACK(on_row_selected), app);

    app->toggle_btn = gtk_button_new_with_label("Show all devices");
    gtk_box_append(GTK_BOX(left_vbox), app->toggle_btn);
    g_signal_connect(app->toggle_btn, "clicked",
        G_CALLBACK(on_toggle_show_all), app);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh live");
    gtk_box_append(GTK_BOX(left_vbox), refresh_btn);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_live), app);

    GtkWidget *load_db_btn = gtk_button_new_with_label("Load from db");
    gtk_box_append(GTK_BOX(left_vbox), load_db_btn);
    g_signal_connect(load_db_btn, "clicked", G_CALLBACK(on_load_db), app);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(left_vbox), sep);

    GtkWidget *load_from_btn = gtk_button_new_with_label("Load from file...");
    gtk_box_append(GTK_BOX(left_vbox), load_from_btn);
    g_signal_connect(load_from_btn, "clicked", G_CALLBACK(on_load_from), app);

    GtkWidget *save_as_btn = gtk_button_new_with_label("Save as file...");
    gtk_box_append(GTK_BOX(left_vbox), save_as_btn);
    g_signal_connect(save_as_btn, "clicked", G_CALLBACK(on_save_as), app);

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

    app->slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(app->slider), FALSE);
    gtk_widget_set_sensitive(app->slider, FALSE);
    gtk_widget_set_hexpand(app->slider, TRUE);
    gtk_scale_add_mark(GTK_SCALE(app->slider),   0, GTK_POS_BOTTOM, "0%");
    gtk_scale_add_mark(GTK_SCALE(app->slider),  25, GTK_POS_BOTTOM, NULL);
    gtk_scale_add_mark(GTK_SCALE(app->slider),  50, GTK_POS_BOTTOM, "50%");
    gtk_scale_add_mark(GTK_SCALE(app->slider),  75, GTK_POS_BOTTOM, NULL);
    gtk_scale_add_mark(GTK_SCALE(app->slider), 100, GTK_POS_BOTTOM, "100%");
    gtk_box_append(GTK_BOX(right_vbox), app->slider);

    g_signal_connect(app->slider, "value-changed",
        G_CALLBACK(on_slider_value_changed), app);

    GtkEventController *legacy = gtk_event_controller_legacy_new();
    g_signal_connect(legacy, "event", G_CALLBACK(on_slider_released), app);
    gtk_widget_add_controller(app->slider, legacy);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-released",
        G_CALLBACK(on_slider_key_released), app);
    gtk_widget_add_controller(app->slider, key_ctrl);

    /* ── Save / Revert buttons ── */
    GtkWidget *action_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(right_vbox), action_hbox);

    app->save_btn = gtk_button_new_with_label("Save to db");
    gtk_widget_add_css_class(app->save_btn, "btn-save");
    gtk_widget_set_sensitive(app->save_btn, FALSE);
    gtk_box_append(GTK_BOX(action_hbox), app->save_btn);
    g_signal_connect(app->save_btn, "clicked", G_CALLBACK(on_save), app);

    app->revert_btn = gtk_button_new_with_label("Revert all");
    gtk_widget_add_css_class(app->revert_btn, "btn-revert");
    gtk_widget_set_sensitive(app->revert_btn, FALSE);
    gtk_box_append(GTK_BOX(action_hbox), app->revert_btn);
    g_signal_connect(app->revert_btn, "clicked", G_CALLBACK(on_revert), app);

    /* ── Status bar ── */
    app->status_lbl = gtk_label_new("");
    gtk_widget_add_css_class(app->status_lbl, "status-label");
    gtk_label_set_xalign(GTK_LABEL(app->status_lbl), 1.0f);
    gtk_widget_set_margin_end(app->status_lbl, 10);
    gtk_widget_set_margin_bottom(app->status_lbl, 6);
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
