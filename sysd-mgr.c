#include <gtk/gtk.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    GtkStatusbar *statusbar;
    GtkListStore *stores[3];               /* underlying list stores (4 cols: name,state,pid,desc) */
    GtkTreeModelFilter *filters[3];        /* filter wrappers used by the tree-views */
    GtkWidget *filter_entry;               /* common filter entry */
    GtkNotebook *notebook;                 /* notebook pointer - used to know active page */
    GtkTreeView *views[3];                 /* treeviews for selection handling */
} AppData;

/* forward declarations (ensure functions used before definition are known) */
static GtkWidget *create_service_list_view(AppData *ad, int idx, GtkListStore **out_store);

static void trim_newline(char *s) {
    size_t n = strlen(s);
    if (n == 0) return;
    if (s[n-1] == '\n') s[n-1] = '\0';
}

/* helper: read a single systemctl show property value for a unit into out (trims newline) */
static void get_unit_property_value(const char *unit, const char *prop, char *out, size_t n) {
    if (!unit || !prop || !out || n == 0) { if (out) out[0] = '\0'; return; }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "systemctl show -p %s --value %s 2>/dev/null", prop, unit);
    FILE *fp = popen(cmd, "r");
    if (!fp) { out[0] = '\0'; return; }
    if (fgets(out, n, fp)) {
        trim_newline(out);
    } else {
        out[0] = '\0';
    }
    pclose(fp);
}

/* Parse a "list-units" line:
   example:
     ssh.service loaded active running OpenSSH Daemon
   Fill columns: name = first token, state = second meaningful state (active), desc = remainder after sub/state tokens.
*/
static void parse_list_units_line(const char *line, char *out_name, size_t nn,
                                  char *out_state, size_t sn, char *out_desc, size_t dn) {
    char buf[4096];
    strncpy(buf, line, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, " \t", &save);
    if (!tok) { out_name[0]=out_state[0]=out_desc[0]='\0'; return; }
    strncpy(out_name, tok, nn-1); out_name[nn-1] = '\0';

    /* next tokens: LOAD, ACTIVE, SUB (we want ACTIVE as state), then remainder is description */
    char *load = strtok_r(NULL, " \t", &save);
    char *active = strtok_r(NULL, " \t", &save);
    char *sub = strtok_r(NULL, " \t", &save);
    if (active) strncpy(out_state, active, sn-1); else out_state[0]='\0';
    out_state[sn-1] = '\0';

    /* remaining text in save (may start with spaces) is description */
    if (save) {
        while (*save == ' ' || *save == '\t') save++;
        strncpy(out_desc, save, dn-1);
        out_desc[dn-1] = '\0';
    } else {
        out_desc[0] = '\0';
    }
}

/* Parse a "list-unit-files" line:
   example:
     apache2.service                       enabled
   Fill columns: name = first token, state = last token, desc = empty.
*/
static void parse_list_unit_files_line(const char *line, char *out_name, size_t nn,
                                       char *out_state, size_t sn, char *out_desc, size_t dn) {
    char buf[4096];
    strncpy(buf, line, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';

    /* tokenize and remember last token */
    char *save = NULL;
    char *tok = strtok_r(buf, " \t", &save);
    if (!tok) { out_name[0]=out_state[0]=out_desc[0]='\0'; return; }
    strncpy(out_name, tok, nn-1); out_name[nn-1] = '\0';

    char *last = NULL;
    while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
        last = tok;
    }
    if (last) strncpy(out_state, last, sn-1); else out_state[0] = '\0';
    out_state[sn-1] = '\0';
    out_desc[0] = '\0';
}

/* populate a GtkListStore (4 columns: name,state,pid,desc) by running `cmd`.
   mode: 0=list-units (parse units), 1=list-unit-files (parse unit-files) */
static void populate_store_parsed(GtkListStore *store, const char *cmd, int mode) {
    if (!store || !cmd) return;
    gtk_list_store_clear(store);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        GtkTreeIter iter;
        gtk_list_store_insert_with_values(store, &iter, -1,
                                          0, "Error running command",
                                          1, "",
                                          2, "",
                                          3, "",
                                          -1);
        return;
    }

    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        trim_newline(buf);
        if (buf[0] == '\0') continue;

        char name[512] = {0}, state[128] = {0}, desc[2048] = {0}, pid[64] = {0};
        if (mode == 1) {
            parse_list_unit_files_line(buf, name, sizeof(name), state, sizeof(state), desc, sizeof(desc));
        } else {
            /* mode 0 and others: list-units parsing */
            parse_list_units_line(buf, name, sizeof(name), state, sizeof(state), desc, sizeof(desc));
        }

        /* If parsing failed to extract name, fall back to full line in name column */
        if (name[0] == '\0') {
            strncpy(name, buf, sizeof(name)-1);
            name[sizeof(name)-1] = '\0';
        }

        /* If description is empty (common with list-unit-files), fetch it */
        if (desc[0] == '\0') {
            get_unit_property_value(name, "Description", desc, sizeof(desc));
        }

        /* Fetch MainPID (may be "0" if not running); treat "0" as empty */
        get_unit_property_value(name, "MainPID", pid, sizeof(pid));
        if (pid[0] == '0' && pid[1] == '\0') pid[0] = '\0';

        GtkTreeIter iter;
        gtk_list_store_insert_with_values(store, &iter, -1,
                                          0, name,
                                          1, state,
                                          2, pid,
                                          3, desc,
                                          -1);
    }
    pclose(fp);
}

/* Filter helper data */
typedef struct {
    AppData *ad;
    int idx;
} FilterData;

/* visible_func for GtkTreeModelFilter: matches filter_entry text against name/desc/pid/state */
static gboolean service_filter_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    FilterData *fd = (FilterData *)data;
    if (!fd || !fd->ad || !fd->ad->filter_entry) return TRUE;

    const gchar *filter_txt = gtk_entry_get_text(GTK_ENTRY(fd->ad->filter_entry));
    if (!filter_txt || filter_txt[0] == '\0') return TRUE; /* no filter -> show all */

    gchar *name = NULL, *state = NULL, *pid = NULL, *desc = NULL;
    gtk_tree_model_get(model, iter,
                       0, &name,
                       1, &state,
                       2, &pid,
                       3, &desc,
                       -1);

    gboolean match = FALSE;
    gchar *flt_low = g_utf8_strdown(filter_txt, -1);

    if (name && !match) {
        gchar *s = g_utf8_strdown(name, -1);
        if (g_strstr_len(s, -1, flt_low)) match = TRUE;
        g_free(s);
    }
    if (desc && !match) {
        gchar *s = g_utf8_strdown(desc, -1);
        if (g_strstr_len(s, -1, flt_low)) match = TRUE;
        g_free(s);
    }
    if (pid && !match) {
        gchar *s = g_utf8_strdown(pid, -1);
        if (g_strstr_len(s, -1, flt_low)) match = TRUE;
        g_free(s);
    }
    if (state && !match) {
        gchar *s = g_utf8_strdown(state, -1);
        if (g_strstr_len(s, -1, flt_low)) match = TRUE;
        g_free(s);
    }

    g_free(flt_low);
    g_free(name); g_free(state); g_free(pid); g_free(desc);

    return match;
}

/* When filter text changes, refilter all views */
static void on_filter_changed(GtkEntry *entry, gpointer user_data) {
    AppData *ad = (AppData *)user_data;
    if (!ad) return;
    for (int i = 0; i < 3; ++i) {
        if (ad->filters[i]) {
            gtk_tree_model_filter_refilter(ad->filters[i]);
        }
    }
}

/* return newly allocated unit name (caller must g_free), or NULL if none selected */
static gchar *get_selected_unit(AppData *ad) {
    if (!ad || !ad->notebook) return NULL;
    gint page = gtk_notebook_get_current_page(ad->notebook);
    if (page < 0 || page > 2) return NULL;
    GtkTreeView *tv = ad->views[page];
    if (!tv) return NULL;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return NULL;

    gchar *name = NULL;
    gtk_tree_model_get(model, &iter, 0, &name, -1);
    return name; /* g_strdup returned by gtk */
}

/* GUI password prompt. Returns newly allocated password (caller must g_free) or NULL on cancel. */
static gchar *prompt_for_password(GtkWindow *parent) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Authentication required",
                                                 parent,
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 "_OK", GTK_RESPONSE_OK,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 0);

    GtkWidget *lbl = gtk_label_new("Enter sudo password:");
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    gtk_widget_grab_focus(entry);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gchar *pwd = NULL;
    if (resp == GTK_RESPONSE_OK) {
        const gchar *txt = gtk_entry_get_text(GTK_ENTRY(entry));
        if (txt && txt[0] != '\0') pwd = g_strdup(txt);
    }
    gtk_widget_destroy(dlg);
    return pwd;
}

/* run a command via pkexec (preferred). Returns TRUE on success, FALSE otherwise.
   If provided, out_stderr will be set to newly allocated string with stderr (caller must g_free). */
static gboolean run_command_pkexec_and_collect(const char *cmd, gchar **out_stderr) {
    if (out_stderr) *out_stderr = NULL;
    gchar *full = g_strdup_printf("pkexec /bin/sh -c '%s 2>&1 1>/dev/null'", cmd);
    gchar *out = NULL;
    gchar *err = NULL;
    gint status = 0;
    GError *gerr = NULL;
    gboolean ok = g_spawn_command_line_sync(full, &out, &err, &status, &gerr);
    g_free(full);
    if (!ok) {
        if (gerr && out_stderr) {
            *out_stderr = g_strdup(gerr->message);
        }
        g_clear_error(&gerr);
        g_free(out); g_free(err);
        return FALSE;
    }
    /* err contains combined output due to our redirect; use it */
    if (out_stderr && err) *out_stderr = g_strdup(err);
    g_free(out); g_free(err);
    return (status == 0);
}

/* run command with sudo -S by prompting for password and writing to child's stdin.
   Returns TRUE on success. stderr (if non-NULL) will be allocated with command output. */
static gboolean run_command_with_sudo_and_password(const char *cmd, gchar **stderr_out, GtkWindow *parent) {
    if (stderr_out) *stderr_out = NULL;
    gchar *pwd = prompt_for_password(parent);
    if (!pwd) return FALSE;

    /* build argv manually and avoid g_shell_parse_argv (portable and removes prototype mismatch) */
    gchar *inner = g_strdup_printf("%s 2>&1 1>/dev/null", cmd); /* passed to /bin/sh -c */
    gchar *spawn_argv[] = { "sudo", "-S", "/bin/sh", "-c", inner, NULL };

    GPid child_pid = 0;
    gint in_fd = -1, out_fd = -1, err_fd = -1;
    GError *error = NULL;
    if (!g_spawn_async_with_pipes(NULL, spawn_argv, NULL,
                                  G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL,
                                  &child_pid,
                                  &in_fd, &out_fd, &err_fd,
                                  &error)) {
        g_free(inner);
        g_free(pwd);
        g_printerr("spawn error: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        return FALSE;
    }
    g_free(inner);

    /* write password to child's stdin (sudo -S reads from stdin) */
    gchar *pwline = g_strdup_printf("%s\n", pwd);
    ssize_t written = write(in_fd, pwline, strlen(pwline));
    (void)written;
    /* close stdin to signal EOF */
    close(in_fd);
    g_free(pwline);
    g_free(pwd);

    /* read combined output from err_fd (we redirected stderr to stdout in shell) */
    GString *outbuf = g_string_new(NULL);
    char buf[512];
    ssize_t r;
    while ((r = read(err_fd, buf, sizeof(buf))) > 0) {
        g_string_append_len(outbuf, buf, r);
    }
    close(err_fd);
    if (out_fd != -1) close(out_fd);

    /* wait for child */
    int status = 0;
    waitpid(child_pid, &status, 0);

    if (stderr_out && outbuf->len > 0) {
        *stderr_out = g_string_free(outbuf, FALSE);
    } else {
        g_string_free(outbuf, TRUE);
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* central runner: try pkexec first, fallback to sudo prompt. Shows message in statusbar via ad. */
static void run_systemctl_action_and_notify(AppData *ad, const char *cmd) {
    if (!ad) return;
    gchar *err = NULL;
    gboolean ok = run_command_pkexec_and_collect(cmd, &err);
    if (!ok) {
        /* fallback to sudo with password prompt */
        gboolean ok2 = run_command_with_sudo_and_password(cmd, &err, GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(ad->statusbar))));
        if (ok2) ok = TRUE;
    }

    guint ctx = gtk_statusbar_get_context_id(ad->statusbar, "action");
    gtk_statusbar_pop(ad->statusbar, ctx);
    if (ok) {
        gtk_statusbar_push(ad->statusbar, ctx, "Action completed successfully");
    } else {
        if (err && err[0] != '\0') {
            gtk_statusbar_push(ad->statusbar, ctx, err);
        } else {
            gtk_statusbar_push(ad->statusbar, ctx, "Action failed");
        }
    }
    g_free(err);

    /* refresh lists after any control action */
    /* reuse the same populators used elsewhere */
    populate_store_parsed(ad->stores[0],
        "systemctl --no-legend --no-pager list-units --type=service --state=running", 0);
    populate_store_parsed(ad->stores[1],
        "systemctl --no-legend --no-pager list-unit-files --type=service --state=enabled", 1);
    populate_store_parsed(ad->stores[2],
        "systemctl --no-legend --no-pager list-units --type=service --all", 0);

    /* re-apply filter */
    on_filter_changed(GTK_ENTRY(ad->filter_entry), ad);
}

/* callbacks for control buttons */
static void on_start_clicked(GtkButton *btn, gpointer user_data) {
    AppData *ad = (AppData *)user_data;
    gchar *unit = get_selected_unit(ad);
    if (!unit) {
        guint ctx = gtk_statusbar_get_context_id(ad->statusbar, "action");
        gtk_statusbar_push(ad->statusbar, ctx, "No service selected");
        return;
    }
    gchar *cmd = g_strdup_printf("systemctl start %s", unit);
    run_systemctl_action_and_notify(ad, cmd);
    g_free(cmd); g_free(unit);
}

static void on_stop_clicked(GtkButton *btn, gpointer user_data) {
    AppData *ad = (AppData *)user_data;
    gchar *unit = get_selected_unit(ad);
    if (!unit) {
        guint ctx = gtk_statusbar_get_context_id(ad->statusbar, "action");
        gtk_statusbar_push(ad->statusbar, ctx, "No service selected");
        return;
    }
    gchar *cmd = g_strdup_printf("systemctl stop %s", unit);
    run_systemctl_action_and_notify(ad, cmd);
    g_free(cmd); g_free(unit);
}

static void on_restart_clicked(GtkButton *btn, gpointer user_data) {
    AppData *ad = (AppData *)user_data;
    gchar *unit = get_selected_unit(ad);
    if (!unit) {
        guint ctx = gtk_statusbar_get_context_id(ad->statusbar, "action");
        gtk_statusbar_push(ad->statusbar, ctx, "No service selected");
        return;
    }
    gchar *cmd = g_strdup_printf("systemctl restart %s", unit);
    run_systemctl_action_and_notify(ad, cmd);
    g_free(cmd); g_free(unit);
}

static void on_reload_clicked(GtkButton *btn, gpointer user_data) {
    AppData *ad = (AppData *)user_data;
    /* reload unit if selected, otherwise do daemon-reload */
    gchar *unit = get_selected_unit(ad);
    gchar *cmd;
    if (unit) {
        cmd = g_strdup_printf("systemctl reload %s", unit);
    } else {
        cmd = g_strdup_printf("systemctl daemon-reload");
    }
    run_systemctl_action_and_notify(ad, cmd);
    g_free(cmd); g_free(unit);
}

/* toggle enable/disable */
static void on_toggle_enable_toggled(GtkToggleButton *tb, gpointer user_data) {
    AppData *ad = (AppData *)user_data;
    gboolean active = gtk_toggle_button_get_active(tb);
    gchar *unit = get_selected_unit(ad);
    if (!unit) {
        guint ctx = gtk_statusbar_get_context_id(ad->statusbar, "action");
        gtk_statusbar_push(ad->statusbar, ctx, "No service selected");
        gtk_toggle_button_set_active(tb, !active); /* revert */
        return;
    }
    gchar *cmd = g_strdup_printf("systemctl %s %s", active ? "enable" : "disable", unit);
    run_systemctl_action_and_notify(ad, cmd);
    g_free(cmd); g_free(unit);
}

/* populate a GtkListStore (4 columns: name,state,pid,desc) by running `cmd`.
   mode: 0=list-units (parse units), 1=list-unit-files (parse unit-files) */
static void on_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    AppData *ad = (AppData *)user_data;
    if (!ad) return;

    const gchar *msgs[] = {
        "Showing: Services currently running",
        "Showing: Services enabled at boot",
        "Showing: All services"
    };

    guint ctx = gtk_statusbar_get_context_id(ad->statusbar, "status");
    gtk_statusbar_pop(ad->statusbar, ctx);
    gtk_statusbar_push(ad->statusbar, ctx, msgs[page_num]);

    /* Refresh the list for the active page */
    switch (page_num) {
        case 0:
            populate_store_parsed(ad->stores[0],
                "systemctl --no-legend --no-pager list-units --type=service --state=running", 0);
            break;
        case 1:
            populate_store_parsed(ad->stores[1],
                "systemctl --no-legend --no-pager list-unit-files --type=service --state=enabled", 1);
            break;
        case 2:
            populate_store_parsed(ad->stores[2],
                "systemctl --no-legend --no-pager list-units --type=service --all", 0);
            break;
        default:
            break;
    }
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "SysD Manager");
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 550);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* --- Menu bar --- */
    GtkWidget *menubar = gtk_menu_bar_new();

    /* File menu */
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect_swapped(quit_item, "activate", G_CALLBACK(gtk_widget_destroy), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    /* Help menu */
    GtkWidget *help_item = gtk_menu_item_new_with_label("Help");
    GtkWidget *help_menu = gtk_menu_new();
    GtkWidget *about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(about_item, "activate", G_CALLBACK(gtk_show_about_dialog), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_item);

    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* --- Filter row (new) --- */
    AppData *ad = g_new0(AppData, 1);
    GtkWidget *filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(filter_box, 6);
    gtk_widget_set_margin_bottom(filter_box, 6);
    gtk_widget_set_margin_start(filter_box, 6);
    gtk_widget_set_margin_end(filter_box, 6);

    GtkWidget *filter_label = gtk_label_new("Filter:");
    gtk_box_pack_start(GTK_BOX(filter_box), filter_label, FALSE, FALSE, 0);

    GtkWidget *filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(filter_entry), "type substring to match service name, description, pid or state");
    gtk_box_pack_start(GTK_BOX(filter_box), filter_entry, TRUE, TRUE, 0);
    ad->filter_entry = filter_entry;
    g_signal_connect(filter_entry, "changed", G_CALLBACK(on_filter_changed), ad);

    gtk_box_pack_start(GTK_BOX(vbox), filter_box, FALSE, FALSE, 0);

    /* --- Notebook with three tabs --- */
    GtkWidget *notebook = gtk_notebook_new();

    /* create views: pass AppData and index so filter can reference entry */
    GtkWidget *sc1 = create_service_list_view(ad, 0, &ad->stores[0]);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sc1, gtk_label_new("Running"));

    GtkWidget *sc2 = create_service_list_view(ad, 1, &ad->stores[1]);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sc2, gtk_label_new("Enabled at Boot"));

    GtkWidget *sc3 = create_service_list_view(ad, 2, &ad->stores[2]);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sc3, gtk_label_new("All Services"));

    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

    /* --- Control bar --- */
    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(ctrl_box, 6);
    gtk_widget_set_margin_bottom(ctrl_box, 6);
    gtk_widget_set_margin_start(ctrl_box, 6);
    gtk_widget_set_margin_end(ctrl_box, 6);

    GtkWidget *btn_start = gtk_button_new_with_label("Start");
    GtkWidget *btn_stop = gtk_button_new_with_label("Stop");
    GtkWidget *btn_restart = gtk_button_new_with_label("Restart");
    GtkWidget *btn_reload = gtk_button_new_with_label("Reload");

    GtkWidget *enable_toggle = gtk_toggle_button_new_with_label("Enable at boot");

    gtk_box_pack_start(GTK_BOX(ctrl_box), btn_start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), btn_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), btn_restart, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), enable_toggle, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(ctrl_box), btn_reload, FALSE, FALSE, 0);

    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), ad);
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), ad);
    g_signal_connect(btn_restart, "clicked", G_CALLBACK(on_restart_clicked), ad);
    g_signal_connect(btn_reload, "clicked", G_CALLBACK(on_reload_clicked), ad);
    g_signal_connect(enable_toggle, "toggled", G_CALLBACK(on_toggle_enable_toggled), ad);

    gtk_box_pack_start(GTK_BOX(vbox), ctrl_box, FALSE, FALSE, 0);

    /* --- Status bar --- */
    GtkWidget *statusbar = gtk_statusbar_new();
    ad->statusbar = GTK_STATUSBAR(statusbar);
    guint ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(statusbar), "status");
    gtk_statusbar_push(GTK_STATUSBAR(statusbar), ctx, "SysD Manager - ready");
    gtk_box_pack_end(GTK_BOX(vbox), statusbar, FALSE, FALSE, 0);

    /* Connect notebook page switch to update status bar and refresh lists. */
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), ad);

    /* Initial population: populate all three lists */
    populate_store_parsed(ad->stores[0],
        "systemctl --no-legend --no-pager list-units --type=service --state=running", 0);
    populate_store_parsed(ad->stores[1],
        "systemctl --no-legend --no-pager list-unit-files --type=service --state=enabled", 1);
    populate_store_parsed(ad->stores[2],
        "systemctl --no-legend --no-pager list-units --type=service --all", 0);

    /* ensure filter is applied against the initial content */
    on_filter_changed(GTK_ENTRY(ad->filter_entry), ad);

    gtk_widget_show_all(win);
}

static GtkWidget *create_service_list_view(AppData *ad, int idx, GtkListStore **out_store) {
    GtkListStore *store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    if (out_store) *out_store = store;

    /* filter model */
    FilterData *fd = g_new0(FilterData, 1);
    fd->ad = ad;
    fd->idx = idx;
    GtkTreeModelFilter *filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL));
    gtk_tree_model_filter_set_visible_func(filter, service_filter_visible, fd, NULL);
    ad->filters[idx] = filter;

    /* tree view backed by the filter */
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(filter));
    ad->views[idx] = GTK_TREE_VIEW(tree);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);

    GtkCellRenderer *r = gtk_cell_renderer_text_new();

    GtkTreeViewColumn *c_name = gtk_tree_view_column_new_with_attributes("Name", r, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_name);
    gtk_tree_view_column_set_expand(c_name, TRUE);

    GtkTreeViewColumn *c_state = gtk_tree_view_column_new_with_attributes("State", r, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_state);
    gtk_tree_view_column_set_sizing(c_state, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c_state, 120);

    GtkTreeViewColumn *c_pid = gtk_tree_view_column_new_with_attributes("PID", r, "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_pid);
    gtk_tree_view_column_set_sizing(c_pid, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c_pid, 80);

    GtkTreeViewColumn *c_desc = gtk_tree_view_column_new_with_attributes("Description", r, "text", 3, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_desc);
    gtk_tree_view_column_set_expand(c_desc, TRUE);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), tree);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_margin_top(scrolled, 8);
    gtk_widget_set_margin_bottom(scrolled, 8);

    return scrolled;
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("org.example.sysd", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
