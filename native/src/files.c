#include "files.h"

#include <string.h>

/* ---- list item object ---------------------------------------------------------------------------- */

#define SD_TYPE_ENTRY_OBJ (sd_entry_obj_get_type())
G_DECLARE_FINAL_TYPE(SdEntryObj, sd_entry_obj, SD, ENTRY_OBJ, GObject)
struct _SdEntryObj { GObject parent; SdEntry *e; };
G_DEFINE_FINAL_TYPE(SdEntryObj, sd_entry_obj, G_TYPE_OBJECT)
static void sd_entry_obj_finalize(GObject *o) { sd_entry_free(SD_ENTRY_OBJ(o)->e); G_OBJECT_CLASS(sd_entry_obj_parent_class)->finalize(o); }
static void sd_entry_obj_class_init(SdEntryObjClass *k) { G_OBJECT_CLASS(k)->finalize = sd_entry_obj_finalize; }
static void sd_entry_obj_init(SdEntryObj *o) { (void)o; }

struct SdFiles {
    SdXfer *xfer;
    GtkWindow *parent;
    GtkWidget *root, *pathbox, *status, *view;
    GtkCheckButton *follow;
    GListStore *store;
    GtkMultiSelection *sel;
    char *path, *pending_cwd;
    gboolean ready, osc7_seen, active, polling;
    gint64 last_poll;
    guint wait_src, poll_src;
    gboolean alive;
};

static void set_status(SdFiles *f, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_status(SdFiles *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(f->status), m);
    g_free(m);
}

static char *fmt_size(gint64 n) {
    double v = (double)n;
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    return i == 0 ? g_strdup_printf("%d B", (int)v) : g_strdup_printf("%.1f %s", v, u[i]);
}

static char *join(const char *dir, const char *name) {
    if (g_str_equal(dir, "/")) return g_strconcat("/", name, NULL);
    return g_str_has_suffix(dir, "/") ? g_strconcat(dir, name, NULL) : g_strconcat(dir, "/", name, NULL);
}

/* ---- async helper -------------------------------------------------------------------------------------- */

typedef gpointer (*WorkFn)(SdFiles *f, gpointer arg, GError **err);
typedef void (*DoneFn)(SdFiles *f, gpointer result, GError *err, gpointer arg);

typedef struct { SdFiles *f; WorkFn work; DoneFn done; gpointer arg; GDestroyNotify free_arg; } Job;

static void job_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    Job *j = data;
    GError *err = NULL;
    gpointer r = j->work(j->f, j->arg, &err);
    if (err) g_task_return_error(task, err);
    else g_task_return_pointer(task, r, NULL);
}

static void job_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    Job *j = data;
    GError *err = NULL;
    gpointer r = g_task_propagate_pointer(G_TASK(res), &err);
    if (j->f->alive && j->done) j->done(j->f, r, err, j->arg);
    else if (r && j->done == NULL) g_free(r);
    g_clear_error(&err);
    if (j->free_arg) j->free_arg(j->arg);
    g_free(j);
}

static void run_job(SdFiles *f, WorkFn work, DoneFn done, gpointer arg, GDestroyNotify free_arg) {
    Job *j = g_new0(Job, 1);
    j->f = f; j->work = work; j->done = done; j->arg = arg; j->free_arg = free_arg;
    GTask *t = g_task_new(NULL, NULL, job_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, job_thread);
    g_object_unref(t);
}

/* ---- listing ---------------------------------------------------------------------------------------------- */

typedef struct { char *path; GPtrArray *entries; } Listing;

static void listing_free(Listing *l) { if (l) { g_free(l->path); if (l->entries) g_ptr_array_free(l->entries, TRUE); g_free(l); } }

static gpointer work_list(SdFiles *f, gpointer arg, GError **err) {
    char *want = arg;
    char *path = want ? g_strdup(want) : sd_xfer_home(f->xfer, err);
    if (!path) return NULL;
    GPtrArray *e = sd_xfer_list(f->xfer, path, err);
    if (!e) { g_free(path); return NULL; }
    Listing *l = g_new0(Listing, 1);
    l->path = path;
    l->entries = e;
    return l;
}

static void navigate_to(SdFiles *f, const char *path);

static void done_list(SdFiles *f, gpointer result, GError *err, gpointer arg) {
    (void)arg;
    if (err) { set_status(f, "⚠ %s", err->message); return; }
    Listing *l = result;
    g_free(f->path);
    f->path = g_strdup(l->path);
    gtk_editable_set_text(GTK_EDITABLE(f->pathbox), l->path);
    g_list_store_remove_all(f->store);
    for (guint i = 0; i < l->entries->len; i++) {
        SdEntryObj *o = g_object_new(SD_TYPE_ENTRY_OBJ, NULL);
        o->e = l->entries->pdata[i];
        l->entries->pdata[i] = NULL;   /* ownership moves to the object */
        g_list_store_append(f->store, o);
        g_object_unref(o);
    }
    char *label = sd_xfer_label(f->xfer);
    set_status(f, "%u item(s) · %s", l->entries->len, label);
    g_free(label);
    g_ptr_array_set_size(l->entries, 0);
    listing_free(l);
    if (f->pending_cwd && !g_str_equal(f->pending_cwd, f->path) && gtk_check_button_get_active(f->follow)) {
        char *next = f->pending_cwd;
        f->pending_cwd = NULL;
        navigate_to(f, next);
        g_free(next);
    }
}

static void navigate_to(SdFiles *f, const char *path) {
    if (!f->ready) return;
    run_job(f, work_list, done_list, path ? g_strdup(path) : NULL, g_free);
}

void sd_files_navigate(SdFiles *f, const char *path) { navigate_to(f, path); }
const char *sd_files_path(SdFiles *f) { return f->path; }
SdXfer *sd_files_xfer(SdFiles *f) { return f->xfer; }
GtkWidget *sd_files_widget(SdFiles *f) { return f->root; }
GtkCheckButton *sd_files_follow_check(SdFiles *f) { return f->follow; }

/* ---- connection readiness ------------------------------------------------------------------------------------ */

static gpointer work_ready(SdFiles *f, gpointer arg, GError **err) {
    (void)arg; (void)err;
    return GINT_TO_POINTER(sd_xfer_ready(f->xfer) ? 1 : 0);
}

static void done_ready(SdFiles *f, gpointer r, GError *err, gpointer arg) {
    (void)err; (void)arg;
    if (!GPOINTER_TO_INT(r) || f->ready) return;
    f->ready = TRUE;
    if (f->wait_src) { g_source_remove(f->wait_src); f->wait_src = 0; }
    set_status(f, "Connected");
    navigate_to(f, NULL);
}

static gboolean wait_tick(gpointer data) {
    SdFiles *f = data;
    if (f->ready) { f->wait_src = 0; return G_SOURCE_REMOVE; }
    run_job(f, work_ready, done_ready, NULL, NULL);
    return G_SOURCE_CONTINUE;
}

/* ---- follow the terminal's directory ---------------------------------------------------------------------------- */

static gpointer work_cwd(SdFiles *f, gpointer arg, GError **err) { (void)arg; (void)err; return sd_xfer_terminal_cwd(f->xfer); }

static void done_cwd(SdFiles *f, gpointer r, GError *err, gpointer arg) {
    (void)err; (void)arg;
    f->polling = FALSE;
    char *cwd = r;
    if (cwd && f->path && !g_str_equal(cwd, f->path)) navigate_to(f, cwd);
    g_free(cwd);
}

static void poll_cwd(SdFiles *f, gboolean force) {
    if (!f->ready || f->polling || f->osc7_seen || sd_xfer_is_ftp(f->xfer) || !gtk_check_button_get_active(f->follow)) return;
    gint64 now = g_get_monotonic_time();
    if (!force) {
        if (!gtk_widget_get_mapped(f->root)) return;   /* each poll is an ssh round trip: skip while hidden */
        if (!f->active && now - f->last_poll < 10 * G_USEC_PER_SEC) return;   /* and while the shell is quiet */
    }
    f->active = FALSE;
    f->last_poll = now;
    f->polling = TRUE;
    run_job(f, work_cwd, done_cwd, NULL, NULL);
}

static gboolean poll_tick(gpointer data) { poll_cwd(data, FALSE); return G_SOURCE_CONTINUE; }

void sd_files_note_activity(SdFiles *f) { f->active = TRUE; }

void sd_files_set_terminal_cwd(SdFiles *f, const char *path) {
    f->osc7_seen = TRUE;
    if (f->poll_src) { g_source_remove(f->poll_src); f->poll_src = 0; }
    if (gtk_check_button_get_active(f->follow) && (!f->path || !g_str_equal(path, f->path))) {
        g_free(f->pending_cwd);
        f->pending_cwd = g_strdup(path);
        if (f->ready) navigate_to(f, path);
    }
}

static void on_follow_toggled(GtkCheckButton *b, gpointer data) {
    SdFiles *f = data;
    if (gtk_check_button_get_active(b)) poll_cwd(f, TRUE);
}

static void on_map(GtkWidget *w, gpointer data) { (void)w; poll_cwd(data, TRUE); }

/* ---- operations ----------------------------------------------------------------------------------------------------- */

typedef struct { char *a, *b; gboolean flag; GPtrArray *list; } OpArg;
static void oparg_free(gpointer p) { OpArg *o = p; g_free(o->a); g_free(o->b); if (o->list) g_ptr_array_free(o->list, TRUE); g_free(o); }

static void done_simple_refresh(SdFiles *f, gpointer r, GError *err, gpointer arg) {
    (void)r; (void)arg;
    if (err) set_status(f, "⚠ %s", err->message);
    else navigate_to(f, f->path);
}

static gpointer work_mkdir(SdFiles *f, gpointer arg, GError **err) { OpArg *o = arg; return sd_xfer_mkdir(f->xfer, o->a, err) ? GINT_TO_POINTER(1) : NULL; }
static gpointer work_rename(SdFiles *f, gpointer arg, GError **err) { OpArg *o = arg; return sd_xfer_rename(f->xfer, o->a, o->b, err) ? GINT_TO_POINTER(1) : NULL; }

typedef struct { char *path; gboolean is_dir; } DelItem;
static gpointer work_delete(SdFiles *f, gpointer arg, GError **err) {
    OpArg *o = arg;
    for (guint i = 0; i < o->list->len; i++) {
        DelItem *d = o->list->pdata[i];
        if (!sd_xfer_remove(f->xfer, d->path, d->is_dir, err)) return NULL;
    }
    return GINT_TO_POINTER(1);
}

static void del_item_free(gpointer p) { DelItem *d = p; g_free(d->path); g_free(d); }

static void done_download(SdFiles *f, gpointer r, GError *err, gpointer arg) {
    (void)r;
    OpArg *o = arg;
    if (err) set_status(f, "⚠ %s", err->message);
    else set_status(f, "Downloaded to %s", o->b);
}

static gpointer work_download(SdFiles *f, gpointer arg, GError **err) {
    OpArg *o = arg;   /* list holds DelItem (path, is_dir); b is the destination folder */
    for (guint i = 0; i < o->list->len; i++) {
        DelItem *d = o->list->pdata[i];
        if (!sd_xfer_download(f->xfer, d->path, o->b, d->is_dir, err)) return NULL;
    }
    return GINT_TO_POINTER(1);
}

static void done_upload(SdFiles *f, gpointer r, GError *err, gpointer arg) {
    (void)r; (void)arg;
    if (err) set_status(f, "⚠ %s", err->message);
    else { set_status(f, "Upload complete"); navigate_to(f, f->path); }
}

static gpointer work_upload(SdFiles *f, gpointer arg, GError **err) {
    OpArg *o = arg;   /* list holds local paths (char*), a is the target folder */
    for (guint i = 0; i < o->list->len; i++)
        if (!sd_xfer_upload(f->xfer, o->list->pdata[i], o->a, err)) return NULL;
    return GINT_TO_POINTER(1);
}

static void upload_paths(SdFiles *f, GPtrArray *paths) {   /* takes ownership of paths (free func g_free) */
    if (!f->ready || !f->path) { g_ptr_array_free(paths, TRUE); return; }
    OpArg *o = g_new0(OpArg, 1);
    o->a = g_strdup(f->path);
    o->list = paths;
    set_status(f, "Uploading %u item(s)…", paths->len);
    run_job(f, work_upload, done_upload, o, oparg_free);
}

static GPtrArray *selected_entries(SdFiles *f) {   /* SdEntry* (borrowed) */
    GPtrArray *out = g_ptr_array_new();
    GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(f->sel));
    GtkBitsetIter it;
    guint pos;
    if (gtk_bitset_iter_init_first(&it, bs, &pos))
        do {
            SdEntryObj *o = g_list_model_get_item(G_LIST_MODEL(f->sel), pos);
            if (o) { g_ptr_array_add(out, o->e); g_object_unref(o); }   /* the model still owns it */
        } while (gtk_bitset_iter_next(&it, &pos));
    gtk_bitset_unref(bs);
    return out;
}

static void download_entries(SdFiles *f, GPtrArray *entries, const char *dest) {
    OpArg *o = g_new0(OpArg, 1);
    o->b = g_strdup(dest);
    o->list = g_ptr_array_new_with_free_func(del_item_free);
    for (guint i = 0; i < entries->len; i++) {
        SdEntry *e = entries->pdata[i];
        DelItem *d = g_new0(DelItem, 1);
        d->path = join(f->path, e->name);
        d->is_dir = e->is_dir;
        g_ptr_array_add(o->list, d);
    }
    set_status(f, "Downloading %u item(s)…", entries->len);
    run_job(f, work_download, done_download, o, oparg_free);
}

static void on_download_folder(GObject *src, GAsyncResult *res, gpointer data) {
    SdFiles *f = data;
    GFile *dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (dir) {
        GPtrArray *sel = selected_entries(f);
        char *dest = g_file_get_path(dir);
        if (sel->len) download_entries(f, sel, dest);
        g_free(dest);
        g_ptr_array_free(sel, TRUE);
        g_object_unref(dir);
    }
}

static void action_download(SdFiles *f) {
    GPtrArray *sel = selected_entries(f);
    if (!sel->len) { set_status(f, "Select something to download first"); g_ptr_array_free(sel, TRUE); return; }
    g_ptr_array_free(sel, TRUE);
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Download to");
    gtk_file_dialog_select_folder(d, f->parent, NULL, on_download_folder, f);
    g_object_unref(d);
}

static void on_upload_files(GObject *src, GAsyncResult *res, gpointer data) {
    SdFiles *f = data;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!files) return;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        GFile *gf = g_list_model_get_item(files, i);
        g_ptr_array_add(paths, g_file_get_path(gf));
        g_object_unref(gf);
    }
    g_object_unref(files);
    upload_paths(f, paths);
}

static void action_upload(SdFiles *f) {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Upload files");
    gtk_file_dialog_open_multiple(d, f->parent, NULL, on_upload_files, f);
    g_object_unref(d);
}

/* ---- small prompt / confirm dialogs ---------------------------------------------------------------------------------- */

typedef struct { SdFiles *f; GtkWidget *entry; GtkWidget *win; char *old; int kind; } PromptCtx;

static void prompt_ok(GtkButton *b, gpointer data) {
    (void)b;
    PromptCtx *p = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(p->entry));
    if (*text && p->f->path) {
        OpArg *o = g_new0(OpArg, 1);
        if (p->kind == 0) {
            o->a = join(p->f->path, text);
            run_job(p->f, work_mkdir, done_simple_refresh, o, oparg_free);
        } else if (!g_str_equal(text, p->old)) {
            o->a = join(p->f->path, p->old);
            o->b = join(p->f->path, text);
            run_job(p->f, work_rename, done_simple_refresh, o, oparg_free);
        } else {
            g_free(o);
        }
    }
    gtk_window_destroy(GTK_WINDOW(p->win));
}

static void prompt_closed(GtkWidget *w, gpointer data) { (void)w; PromptCtx *p = data; g_free(p->old); g_free(p); }

static void ask_name(SdFiles *f, const char *title, const char *initial, int kind) {
    PromptCtx *p = g_new0(PromptCtx, 1);
    p->f = f; p->kind = kind; p->old = g_strdup(initial);
    GtkWidget *win = gtk_window_new();
    p->win = win;
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win), f->parent);
    gtk_window_set_default_size(GTK_WINDOW(win), 360, -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    p->entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(p->entry), initial ? initial : "");
    GtkWidget *ok = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(ok, "clicked", G_CALLBACK(prompt_ok), p);
    g_signal_connect(p->entry, "activate", G_CALLBACK(prompt_ok), p);
    g_signal_connect(win, "destroy", G_CALLBACK(prompt_closed), p);
    gtk_box_append(GTK_BOX(box), p->entry);
    gtk_box_append(GTK_BOX(box), ok);
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(p->entry);
}

static void on_delete_choice(GObject *src, GAsyncResult *res, gpointer data) {
    SdFiles *f = data;
    GPtrArray *items = g_object_get_data(src, "items");
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    if (choice == 1 && items && items->len) {
        OpArg *o = g_new0(OpArg, 1);
        o->list = g_ptr_array_new_with_free_func(del_item_free);
        for (guint i = 0; i < items->len; i++) {
            DelItem *s = items->pdata[i], *d = g_new0(DelItem, 1);
            d->path = g_strdup(s->path);
            d->is_dir = s->is_dir;
            g_ptr_array_add(o->list, d);
        }
        run_job(f, work_delete, done_simple_refresh, o, oparg_free);
    }
}

static void action_delete(SdFiles *f) {
    GPtrArray *sel = selected_entries(f);
    if (!sel->len) { g_ptr_array_free(sel, TRUE); return; }
    GPtrArray *items = g_ptr_array_new_with_free_func(del_item_free);
    GString *names = g_string_new(NULL);
    for (guint i = 0; i < sel->len; i++) {
        SdEntry *e = sel->pdata[i];
        DelItem *d = g_new0(DelItem, 1);
        d->path = join(f->path, e->name);
        d->is_dir = e->is_dir;
        g_ptr_array_add(items, d);
        if (i < 5) g_string_append_printf(names, "%s%s", i ? ", " : "", e->name);
    }
    char *msg = g_strdup_printf("Delete %u item(s) on the server?", sel->len);
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_set_detail(dlg, names->str);
    const char *buttons[] = {"Cancel", "Delete", NULL};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);
    g_object_set_data_full(G_OBJECT(dlg), "items", items, (GDestroyNotify)g_ptr_array_unref);
    gtk_alert_dialog_choose(dlg, f->parent, NULL, on_delete_choice, f);
    g_object_unref(dlg);
    g_free(msg);
    g_string_free(names, TRUE);
    g_ptr_array_free(sel, TRUE);
}

static void action_rename(SdFiles *f) {
    GPtrArray *sel = selected_entries(f);
    if (sel->len == 1) ask_name(f, "Rename", ((SdEntry *)sel->pdata[0])->name, 1);
    g_ptr_array_free(sel, TRUE);
}

/* ---- widgets ------------------------------------------------------------------------------------------------------------ */

static void on_activate_row(GtkColumnView *v, guint pos, gpointer data) {
    (void)v;
    SdFiles *f = data;
    SdEntryObj *o = g_list_model_get_item(G_LIST_MODEL(f->sel), pos);
    if (!o || !f->path) return;
    SdEntry *e = o->e;
    char *target = join(f->path, e->name);
    if (e->is_dir || e->is_link) {
        navigate_to(f, target);   /* a symlink to a file just fails to list and reports it */
    } else {
        GPtrArray *one = g_ptr_array_new();
        g_ptr_array_add(one, e);
        char *dl = g_build_filename(g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD) ? g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD) : g_get_home_dir(), NULL);
        download_entries(f, one, dl);
        g_free(dl);
        g_ptr_array_free(one, TRUE);
    }
    g_free(target);
    g_object_unref(o);
}

static void setup_name(GtkListItemFactory *fac, GtkListItem *item, gpointer d) {
    (void)fac; (void)d;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(item, box);
}
static void bind_name(GtkListItemFactory *fac, GtkListItem *item, gpointer d) {
    (void)fac; (void)d;
    SdEntryObj *o = gtk_list_item_get_item(item);
    GtkWidget *box = gtk_list_item_get_child(item);
    GtkWidget *icon = gtk_widget_get_first_child(box), *label = gtk_widget_get_last_child(box);
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), (o->e->is_dir || o->e->is_link) ? "folder-symbolic" : "text-x-generic-symbolic");
    gtk_label_set_text(GTK_LABEL(label), o->e->name);
}
static void setup_text(GtkListItemFactory *fac, GtkListItem *item, gpointer d) {
    (void)fac; (void)d;
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), GPOINTER_TO_INT(d) ? 1.0f : 0.0f);
    gtk_widget_add_css_class(l, "sd-muted");
    gtk_list_item_set_child(item, l);
}
static void bind_size(GtkListItemFactory *fac, GtkListItem *item, gpointer d) {
    (void)fac; (void)d;
    SdEntryObj *o = gtk_list_item_get_item(item);
    char *s = o->e->is_dir ? g_strdup("") : fmt_size(o->e->size);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), s);
    g_free(s);
}
static void bind_mod(GtkListItemFactory *fac, GtkListItem *item, gpointer d) {
    (void)fac; (void)d;
    SdEntryObj *o = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), o->e->modified);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *v, double x, double y, gpointer data) {
    (void)t; (void)x; (void)y;
    SdFiles *f = data;
    if (!G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) return FALSE;
    GSList *files = g_value_get_boxed(v);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (GSList *l = files; l; l = l->next) { char *p = g_file_get_path(l->data); if (p) g_ptr_array_add(paths, p); }
    upload_paths(f, paths);
    return TRUE;
}

static void tool_up(GtkButton *b, gpointer d) { (void)b; SdFiles *f = d; if (f->path && !g_str_equal(f->path, "/")) { char *p = g_path_get_dirname(f->path); navigate_to(f, p); g_free(p); } }
static void tool_refresh(GtkButton *b, gpointer d) { (void)b; SdFiles *f = d; navigate_to(f, f->path); }
static void tool_home(GtkButton *b, gpointer d) { (void)b; navigate_to(d, NULL); }
static void on_path_enter(GtkEntry *e, gpointer d) { const char *t = gtk_editable_get_text(GTK_EDITABLE(e)); navigate_to(d, *t ? t : "/"); }

static void menu_cb(GSimpleAction *a, GVariant *p, gpointer data) {
    (void)p;
    SdFiles *f = data;
    const char *n = g_action_get_name(G_ACTION(a));
    if (g_str_equal(n, "mkdir")) ask_name(f, "New folder", "", 0);
    else if (g_str_equal(n, "upload")) action_upload(f);
    else if (g_str_equal(n, "download")) action_download(f);
    else if (g_str_equal(n, "rename")) action_rename(f);
    else if (g_str_equal(n, "delete")) action_delete(f);
    else if (g_str_equal(n, "refresh")) navigate_to(f, f->path);
    else if (g_str_equal(n, "copypath")) {
        GPtrArray *sel = selected_entries(f);
        if (sel->len == 1) { char *p = join(f->path, ((SdEntry *)sel->pdata[0])->name); gdk_clipboard_set_text(gtk_widget_get_clipboard(f->root), p); g_free(p); }
        g_ptr_array_free(sel, TRUE);
    }
}

static void files_destroyed(GtkWidget *w, gpointer data) {
    (void)w;
    SdFiles *f = data;
    f->alive = FALSE;
    if (f->wait_src) g_source_remove(f->wait_src);
    if (f->poll_src) g_source_remove(f->poll_src);
    g_free(f->path); g_free(f->pending_cwd);
    sd_xfer_free(f->xfer);
    g_free(f);
}

SdFiles *sd_files_new(SdXfer *xfer, gboolean follow, GtkWindow *parent) {
    SdFiles *f = g_new0(SdFiles, 1);
    f->xfer = xfer; f->parent = parent; f->alive = TRUE;
    f->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(f->root, 6); gtk_widget_set_margin_end(f->root, 4);
    gtk_widget_set_margin_top(f->root, 4); gtk_widget_set_margin_bottom(f->root, 4);

    GSimpleActionGroup *group = g_simple_action_group_new();
    const char *names[] = {"mkdir", "upload", "download", "rename", "delete", "refresh", "copypath", NULL};
    for (int i = 0; names[i]; i++) {
        GSimpleAction *a = g_simple_action_new(names[i], NULL);
        g_signal_connect(a, "activate", G_CALLBACK(menu_cb), f);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(f->root, "files", G_ACTION_GROUP(group));
    g_object_unref(group);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    struct { const char *icon, *tip; GCallback cb; } tools[] = {
        {"go-up-symbolic", "Parent folder", G_CALLBACK(tool_up)}, {"view-refresh-symbolic", "Refresh", G_CALLBACK(tool_refresh)},
        {"user-home-symbolic", "Home folder", G_CALLBACK(tool_home)},
    };
    for (guint i = 0; i < G_N_ELEMENTS(tools); i++) {
        GtkWidget *b = gtk_button_new_from_icon_name(tools[i].icon);
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_set_tooltip_text(b, tools[i].tip);
        g_signal_connect(b, "clicked", tools[i].cb, f);
        gtk_box_append(GTK_BOX(bar), b);
    }
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "New folder…", "files.mkdir");
    g_menu_append(menu, "Upload files…", "files.upload");
    g_menu_append(menu, "Download selected…", "files.download");
    GMenu *sec = g_menu_new();
    g_menu_append(sec, "Rename…", "files.rename");
    g_menu_append(sec, "Delete…", "files.delete");
    g_menu_append(sec, "Copy path", "files.copypath");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "Actions");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    gtk_box_append(GTK_BOX(bar), mb);
    g_object_unref(menu); g_object_unref(sec);

    f->pathbox = gtk_entry_new();
    g_signal_connect(f->pathbox, "activate", G_CALLBACK(on_path_enter), f);
    f->follow = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow terminal path"));
    gtk_check_button_set_active(f->follow, follow);
    g_signal_connect(f->follow, "toggled", G_CALLBACK(on_follow_toggled), f);

    f->store = g_list_store_new(SD_TYPE_ENTRY_OBJ);
    f->sel = gtk_multi_selection_new(G_LIST_MODEL(f->store));
    f->view = gtk_column_view_new(GTK_SELECTION_MODEL(f->sel));
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(f->view), FALSE);
    GtkListItemFactory *fn = gtk_signal_list_item_factory_new(), *fs = gtk_signal_list_item_factory_new(), *fm = gtk_signal_list_item_factory_new();
    g_signal_connect(fn, "setup", G_CALLBACK(setup_name), NULL);
    g_signal_connect(fn, "bind", G_CALLBACK(bind_name), NULL);
    g_signal_connect(fs, "setup", G_CALLBACK(setup_text), GINT_TO_POINTER(1));
    g_signal_connect(fs, "bind", G_CALLBACK(bind_size), NULL);
    g_signal_connect(fm, "setup", G_CALLBACK(setup_text), GINT_TO_POINTER(0));
    g_signal_connect(fm, "bind", G_CALLBACK(bind_mod), NULL);
    GtkColumnViewColumn *c1 = gtk_column_view_column_new("Name", fn), *c2 = gtk_column_view_column_new("Size", fs),
                        *c3 = gtk_column_view_column_new("Modified", fm);
    gtk_column_view_column_set_expand(c1, TRUE);
    gtk_column_view_column_set_resizable(c1, TRUE);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(f->view), c1);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(f->view), c2);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(f->view), c3);
    g_object_unref(c1); g_object_unref(c2); g_object_unref(c3);
    g_signal_connect(f->view, "activate", G_CALLBACK(on_activate_row), f);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), f);
    gtk_widget_add_controller(f->view, GTK_EVENT_CONTROLLER(drop));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), f->view);
    gtk_widget_set_vexpand(scroll, TRUE);
    f->status = gtk_label_new(sd_xfer_ready(xfer) ? "Connecting…" : "Waiting for the connection…");
    gtk_label_set_xalign(GTK_LABEL(f->status), 0);
    gtk_label_set_wrap(GTK_LABEL(f->status), TRUE);
    gtk_widget_add_css_class(f->status, "sd-status");

    gtk_box_append(GTK_BOX(f->root), bar);
    gtk_box_append(GTK_BOX(f->root), f->pathbox);
    if (!sd_xfer_is_ftp(xfer)) gtk_box_append(GTK_BOX(f->root), GTK_WIDGET(f->follow));
    gtk_box_append(GTK_BOX(f->root), scroll);
    gtk_box_append(GTK_BOX(f->root), f->status);

    g_signal_connect(f->root, "destroy", G_CALLBACK(files_destroyed), f);
    g_signal_connect(f->root, "map", G_CALLBACK(on_map), f);
    f->wait_src = g_timeout_add(1000, wait_tick, f);
    f->poll_src = g_timeout_add(1500, poll_tick, f);
    wait_tick(f);
    return f;
}
