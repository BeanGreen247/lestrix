#define _GNU_SOURCE
#include "xfer.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct SdXfer {
    SdConn *conn;
    char *control_path, *dest;
    gboolean ftp;
    char *password;
    gboolean sftp_ok;          /* ssh: sftp subsystem usable */
    GPtrArray *xfer_order;     /* "scp", "ssh" */
};

void sd_entry_free(SdEntry *e) {
    if (!e) return;
    g_free(e->name); g_free(e->modified); g_free(e->perms);
    g_free(e);
}

static void set_err(GError **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_err(GError **err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_set_error_literal(err, SD_XFER_ERROR, 1, m);
    g_free(m);
}

/* ---- running commands with a deadline ---------------------------------------------------------- */

typedef struct { int status; char *out, *err; gboolean timed_out; } Run;

static void run_free(Run *r) { g_free(r->out); g_free(r->err); }

static void read_available(int fd, GString *dst, gboolean *open) {
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) g_string_append_len(dst, buf, n);
    else if (n == 0 || (errno != EAGAIN && errno != EINTR)) *open = FALSE;
}

/* argv may be NULL-terminated; input (may be NULL) is written to stdin. Returns FALSE if it could not start. */
static gboolean run_cmd(char **argv, const char *input, int timeout_s, Run *r) {
    memset(r, 0, sizeof *r);
    gint in_fd, out_fd, err_fd;
    GPid pid;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid,
                                  &in_fd, &out_fd, &err_fd, NULL))
        return FALSE;
    fcntl(out_fd, F_SETFL, O_NONBLOCK);
    fcntl(err_fd, F_SETFL, O_NONBLOCK);
    GString *out = g_string_new(NULL), *er = g_string_new(NULL);
    gsize in_len = input ? strlen(input) : 0, in_off = 0;
    if (!in_len) { close(in_fd); in_fd = -1; } else fcntl(in_fd, F_SETFL, O_NONBLOCK);
    gboolean out_open = TRUE, err_open = TRUE;
    gint64 deadline = timeout_s > 0 ? g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC : 0;
    signal(SIGPIPE, SIG_IGN);
    while (out_open || err_open) {
        struct pollfd fds[3];
        int n = 0;
        if (out_open) fds[n++] = (struct pollfd){out_fd, POLLIN, 0};
        if (err_open) fds[n++] = (struct pollfd){err_fd, POLLIN, 0};
        int in_idx = -1;
        if (in_fd >= 0) { in_idx = n; fds[n++] = (struct pollfd){in_fd, POLLOUT, 0}; }
        int wait_ms = 1000;
        if (deadline) {
            gint64 left = (deadline - g_get_monotonic_time()) / 1000;
            if (left <= 0) { r->timed_out = TRUE; kill(pid, SIGKILL); break; }
            if (left < wait_ms) wait_ms = (int)left;
        }
        int pr = poll(fds, (nfds_t)n, wait_ms);
        if (pr < 0 && errno != EINTR) break;
        int k = 0;
        if (out_open) { if (fds[k].revents) read_available(out_fd, out, &out_open); k++; }
        if (err_open) { if (fds[k].revents) read_available(err_fd, er, &err_open); k++; }
        if (in_idx >= 0 && fds[in_idx].revents) {
            ssize_t w = write(in_fd, input + in_off, in_len - in_off);
            if (w > 0) in_off += (gsize)w;
            if (w < 0 && errno != EAGAIN && errno != EINTR) in_off = in_len;
            if (in_off >= in_len) { close(in_fd); in_fd = -1; }
        }
    }
    if (in_fd >= 0) close(in_fd);
    close(out_fd);
    close(err_fd);
    int st = 0;
    waitpid(pid, &st, 0);
    g_spawn_close_pid(pid);
    r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    r->out = g_string_free(out, FALSE);
    r->err = g_string_free(er, FALSE);
    return TRUE;
}

/* ---- listing parser -------------------------------------------------------------------------------- */

GPtrArray *sd_parse_ls(const char *text) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)sd_entry_free);
    static GRegex *re;
    if (!re)
        re = g_regex_new("^([-dlbcps][rwxsStT-]{9}[.+@]?)\\s+(?:\\d+|\\?)\\s+\\S+\\s+\\S+\\s+(\\d+)\\s+"
                         "(\\w{3}\\s+\\d+\\s+[\\d:]+)\\s+(.+)$", 0, 0, NULL);
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *line = g_strstrip(*l);
        GMatchInfo *mi;
        if (!g_regex_match(re, line, 0, &mi)) { g_match_info_free(mi); continue; }
        char *perms = g_match_info_fetch(mi, 1), *size = g_match_info_fetch(mi, 2), *mtime = g_match_info_fetch(mi, 3),
             *name = g_match_info_fetch(mi, 4);
        g_match_info_free(mi);
        gboolean is_link = perms[0] == 'l';
        char *arrow = is_link ? strstr(name, " -> ") : NULL;
        if (arrow) *arrow = 0;
        char *cr = strchr(name, '\r');
        if (cr) *cr = 0;
        if (g_str_equal(name, ".") || g_str_equal(name, "..")) { g_free(perms); g_free(size); g_free(mtime); g_free(name); continue; }
        SdEntry *e = g_new0(SdEntry, 1);
        e->name = name; e->perms = perms; e->modified = mtime;
        e->is_dir = perms[0] == 'd'; e->is_link = is_link; e->size = g_ascii_strtoll(size, NULL, 10);
        g_free(size);
        g_ptr_array_add(out, e);
    }
    g_strfreev(lines);
    return out;
}

static int entry_cmp(gconstpointer a, gconstpointer b) {
    const SdEntry *x = *(SdEntry *const *)a, *y = *(SdEntry *const *)b;
    int dx = x->is_dir || x->is_link, dy = y->is_dir || y->is_link;
    if (dx != dy) return dy - dx;
    char *kx = g_utf8_casefold(x->name, -1), *ky = g_utf8_casefold(y->name, -1);
    int c = g_strcmp0(kx, ky);
    g_free(kx); g_free(ky);
    return c;
}

/* ---- quoting --------------------------------------------------------------------------------------------- */

static char *sftp_quote(const char *p) {
    GString *s = g_string_new("\"");
    for (; *p; p++) { if (*p == '"' || *p == '\\') g_string_append_c(s, '\\'); g_string_append_c(s, *p); }
    g_string_append_c(s, '"');
    return g_string_free(s, FALSE);
}

/* classic scp: the remote end is a shell, and the client glob-matches replies, so escape with backslashes */
static char *scp_quote(const char *p) {
    GString *s = g_string_new(NULL);
    for (; *p; p++) {
        if (!g_ascii_isalnum(*p) && !strchr("_./~+:@%-", *p)) g_string_append_c(s, '\\');
        g_string_append_c(s, *p);
    }
    return g_string_free(s, FALSE);
}

/* ---- control path ----------------------------------------------------------------------------------------- */

char *sd_xfer_new_control_path(void) {
    char *dir = g_dir_make_tmp("sd-XXXXXX", NULL);   /* honours TMPDIR; keep it short: unix sockets cap at ~100 chars */
    if (!dir || strlen(dir) > 60) { g_free(dir); dir = g_strdup("/tmp/sd-XXXXXX"); dir = mkdtemp(dir) ? dir : NULL; }
    if (!dir) return NULL;
    chmod(dir, 0700);
    char *p = g_build_filename(dir, "m", NULL);
    g_free(dir);
    return p;
}

void sd_xfer_remove_control_path(const char *path) {
    if (!path) return;
    char *dir = g_path_get_dirname(path);
    g_remove(path);
    g_rmdir(dir);
    g_free(dir);
}

/* ---- SSH backend --------------------------------------------------------------------------------------------- */

SdXfer *sd_xfer_new_ssh(const SdConn *conn, const char *control_path) {
    SdXfer *x = g_new0(SdXfer, 1);
    x->conn = sd_conn_copy(conn);
    x->control_path = g_strdup(control_path);
    x->dest = sd_conn_dest(conn);
    x->sftp_ok = TRUE;
    x->xfer_order = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(x->xfer_order, g_strdup("scp"));
    g_ptr_array_add(x->xfer_order, g_strdup("ssh"));
    return x;
}

void sd_xfer_force_mode(SdXfer *x, const char *mode, const char *order) {
    if (mode) x->sftp_ok = g_str_equal(mode, "sftp");
    if (order) {
        g_ptr_array_set_size(x->xfer_order, 0);
        char **parts = g_strsplit(order, ",", -1);
        for (char **p = parts; *p; p++) g_ptr_array_add(x->xfer_order, g_strdup(*p));
        g_strfreev(parts);
    }
}

void sd_xfer_free(SdXfer *x) {
    if (!x) return;
    sd_conn_free(x->conn);
    g_free(x->control_path); g_free(x->dest); g_free(x->password);
    g_ptr_array_free(x->xfer_order, TRUE);
    g_free(x);
}

gboolean sd_xfer_is_ftp(const SdXfer *x) { return x->ftp; }

char *sd_xfer_label(SdXfer *x) {
    if (x->ftp) return g_strdup(g_str_equal(x->conn->protocol, "ftps") ? "FTPS" : "FTP");
    return g_strdup(x->sftp_ok ? "SFTP" : "ssh (no SFTP on this server)");
}

/* argv fragments common to every client riding the master connection */
static void mux_args(SdXfer *x, GPtrArray *a, const char *port_flag) {
    g_ptr_array_add(a, g_strdup("-o")); g_ptr_array_add(a, g_strdup_printf("ControlPath=%s", x->control_path));
    g_ptr_array_add(a, g_strdup("-o")); g_ptr_array_add(a, g_strdup("ControlMaster=no"));
    g_ptr_array_add(a, g_strdup("-o")); g_ptr_array_add(a, g_strdup("BatchMode=yes"));
    if (!sd_conn_is_live(x->conn)) { g_ptr_array_add(a, g_strdup(port_flag)); g_ptr_array_add(a, g_strdup_printf("%d", x->conn->port)); }
}

static char **finish(GPtrArray *a) { g_ptr_array_add(a, NULL); return (char **)g_ptr_array_free(a, FALSE); }

gboolean sd_xfer_ready(SdXfer *x) {
    if (x->ftp) return TRUE;
    if (!g_file_test(x->control_path, G_FILE_TEST_EXISTS)) return FALSE;
    char *argv[] = {"ssh", "-S", x->control_path, "-O", "check", x->dest, NULL};
    Run r;
    gboolean ok = run_cmd(argv, NULL, 5, &r) && r.status == 0;
    run_free(&r);
    return ok;
}

/* run a remote command over the master connection */
static char *ssh_run(SdXfer *x, const char *command, int timeout_s, GError **err) {
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup("ssh"));
    mux_args(x, a, "-p");
    g_ptr_array_add(a, g_strdup(x->dest));
    g_ptr_array_add(a, g_strdup(command));
    char **argv = finish(a);
    Run r;
    if (!run_cmd(argv, NULL, timeout_s, &r)) { g_strfreev(argv); set_err(err, "could not run ssh"); return NULL; }
    g_strfreev(argv);
    char *out = NULL;
    if (r.timed_out) set_err(err, "timed out");
    else if (r.status != 0) set_err(err, "%s", *g_strstrip(r.err) ? r.err : "command failed");
    else out = g_strdup(r.out);
    run_free(&r);
    return out;
}

/* run an sftp batch; returns NULL with *unavailable set when sftp cannot be used at all */
static char *sftp_batch(SdXfer *x, char **cmds, int timeout_s, gboolean *unavailable, GError **err) {
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup("sftp"));
    mux_args(x, a, "-P");
    g_ptr_array_add(a, g_strdup("-b")); g_ptr_array_add(a, g_strdup("-"));
    g_ptr_array_add(a, g_strdup(x->dest));
    char **argv = finish(a);
    char *input = g_strjoinv("\n", cmds);
    char *input_nl = g_strconcat(input, "\n", NULL);
    g_free(input);
    Run r;
    gboolean started = run_cmd(argv, input_nl, timeout_s, &r);
    g_strfreev(argv);
    g_free(input_nl);
    *unavailable = FALSE;
    if (!started) { *unavailable = TRUE; set_err(err, "the sftp command is not installed"); return NULL; }
    char *out = NULL;
    if (r.timed_out) set_err(err, "timed out");
    else if (r.status != 0) {
        if (strcasestr(r.err, "subsystem")) { *unavailable = TRUE; set_err(err, "SFTP is not enabled on this server"); }
        else set_err(err, "%s", *g_strstrip(r.err) ? r.err : "sftp failed");
    } else out = g_strdup(r.out);
    run_free(&r);
    return out;
}

/* sftp first; NULL + *used_sftp == FALSE means fall back to ssh exec */
static char *try_sftp(SdXfer *x, char **cmds, int timeout_s, gboolean *used, GError **err) {
    *used = FALSE;
    if (!x->sftp_ok) return NULL;
    gboolean unavailable;
    char *out = sftp_batch(x, cmds, timeout_s, &unavailable, err);
    if (unavailable) { x->sftp_ok = FALSE; g_clear_error(err); return NULL; }
    *used = TRUE;
    return out;
}

static char *ssh_home(SdXfer *x, GError **err) {
    char *o = ssh_run(x, "printf %s \"$HOME\"", 10, err);
    if (o && !*o) { g_free(o); o = g_strdup("/"); }
    return o;
}

/* ---- FTP backend (libcurl) --------------------------------------------------------------------------------------- */

static size_t curl_to_string(void *p, size_t sz, size_t n, void *ud) { g_string_append_len(ud, p, (gssize)(sz * n)); return sz * n; }
static size_t curl_to_file(void *p, size_t sz, size_t n, void *ud) { return fwrite(p, sz, n, ud); }
static size_t curl_from_file(void *p, size_t sz, size_t n, void *ud) { return fread(p, sz, n, ud); }

static CURL *ftp_handle(SdXfer *x, const char *path, gboolean dir) {
    CURL *c = curl_easy_init();
    char *enc = curl_easy_escape(c, path, 0);   /* escapes '/' too; undo for path separators */
    GString *clean = g_string_new(NULL);
    for (const char *p = enc; *p; p++) {
        if (!strncmp(p, "%2F", 3)) { g_string_append_c(clean, '/'); p += 2; } else g_string_append_c(clean, *p);
    }
    const char *scheme = g_str_equal(x->conn->protocol, "ftps") ? "ftps" : "ftp";
    char *url = g_strdup_printf("%s://%s:%d%s%s", scheme, x->conn->host, x->conn->port, *clean->str == '/' ? "" : "/",
                                clean->str);
    if (dir && !g_str_has_suffix(url, "/")) { char *u2 = g_strconcat(url, "/", NULL); g_free(url); url = u2; }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERNAME, *x->conn->user ? x->conn->user : "anonymous");
    curl_easy_setopt(c, CURLOPT_PASSWORD, x->password ? x->password : "anonymous@");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FTP_FILEMETHOD, CURLFTPMETHOD_NOCWD);
    if (g_str_equal(x->conn->protocol, "ftps")) curl_easy_setopt(c, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    g_free(url); curl_free(enc); g_string_free(clean, TRUE);
    return c;
}

static gboolean ftp_perform(CURL *c, GError **err) {
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { set_err(err, "%s", curl_easy_strerror(rc)); return FALSE; }
    return TRUE;
}

SdXfer *sd_xfer_new_ftp(const SdConn *conn, const char *password) {
    SdXfer *x = g_new0(SdXfer, 1);
    x->conn = sd_conn_copy(conn);
    if (x->conn->port <= 0 || x->conn->port == 22) x->conn->port = 21;
    x->ftp = TRUE;
    x->password = g_strdup(password);
    x->xfer_order = g_ptr_array_new_with_free_func(g_free);
    return x;
}

static GPtrArray *ftp_list(SdXfer *x, const char *path, GError **err) {
    CURL *c = ftp_handle(x, path, TRUE);
    GString *buf = g_string_new(NULL);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_to_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, buf);
    if (!ftp_perform(c, err)) { g_string_free(buf, TRUE); return NULL; }
    GPtrArray *r = sd_parse_ls(buf->str);
    g_string_free(buf, TRUE);
    return r;
}

static gboolean ftp_quote(SdXfer *x, const char *dir, const char *cmd1, const char *cmd2, GError **err) {
    CURL *c = ftp_handle(x, dir, TRUE);
    struct curl_slist *q = curl_slist_append(NULL, cmd1);
    if (cmd2) q = curl_slist_append(q, cmd2);
    curl_easy_setopt(c, CURLOPT_QUOTE, q);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    gboolean ok = ftp_perform(c, err);
    curl_slist_free_all(q);
    return ok;
}

static gboolean ftp_get_file(SdXfer *x, const char *remote, const char *local, GError **err) {
    FILE *f = fopen(local, "wb");
    if (!f) { set_err(err, "cannot write %s", local); return FALSE; }
    CURL *c = ftp_handle(x, remote, FALSE);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    gboolean ok = ftp_perform(c, err);
    fclose(f);
    if (!ok) g_remove(local);
    return ok;
}

static gboolean ftp_put_file(SdXfer *x, const char *local, const char *remote, GError **err) {
    FILE *f = fopen(local, "rb");
    if (!f) { set_err(err, "cannot read %s", local); return FALSE; }
    struct stat st;
    fstat(fileno(f), &st);
    CURL *c = ftp_handle(x, remote, FALSE);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, curl_from_file);
    curl_easy_setopt(c, CURLOPT_READDATA, f);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
    curl_easy_setopt(c, CURLOPT_FTP_CREATE_MISSING_DIRS, 1L);
    gboolean ok = ftp_perform(c, err);
    fclose(f);
    return ok;
}

static char *join_path(const char *a, const char *b) {
    if (g_str_equal(a, "/")) return g_strconcat("/", b, NULL);
    return g_str_has_suffix(a, "/") ? g_strconcat(a, b, NULL) : g_strconcat(a, "/", b, NULL);
}

static gboolean ftp_remove_tree(SdXfer *x, const char *path, gboolean is_dir, GError **err) {
    char *dir = g_path_get_dirname(path), *base = g_path_get_basename(path);
    if (is_dir) {
        GPtrArray *kids = ftp_list(x, path, err);
        if (!kids) { g_free(dir); g_free(base); return FALSE; }
        for (guint i = 0; i < kids->len; i++) {
            SdEntry *e = kids->pdata[i];
            char *child = join_path(path, e->name);
            gboolean ok = ftp_remove_tree(x, child, e->is_dir, err);
            g_free(child);
            if (!ok) { g_ptr_array_free(kids, TRUE); g_free(dir); g_free(base); return FALSE; }
        }
        g_ptr_array_free(kids, TRUE);
    }
    char *cmd = g_strdup_printf("%s %s", is_dir ? "RMD" : "DELE", path);
    gboolean ok = ftp_quote(x, dir, cmd, NULL, err);
    g_free(cmd); g_free(dir); g_free(base);
    return ok;
}

static gboolean ftp_download_tree(SdXfer *x, const char *remote, const char *local_dir, gboolean is_dir, GError **err) {
    char *base = g_path_get_basename(remote);
    char *target = g_build_filename(local_dir, base, NULL);
    gboolean ok = TRUE;
    if (is_dir) {
        g_mkdir_with_parents(target, 0755);
        GPtrArray *kids = ftp_list(x, remote, err);
        ok = kids != NULL;
        for (guint i = 0; ok && i < kids->len; i++) {
            SdEntry *e = kids->pdata[i];
            char *child = join_path(remote, e->name);
            ok = ftp_download_tree(x, child, target, e->is_dir, err);
            g_free(child);
        }
        if (kids) g_ptr_array_free(kids, TRUE);
    } else {
        ok = ftp_get_file(x, remote, target, err);
    }
    g_free(base); g_free(target);
    return ok;
}

static gboolean ftp_upload_tree(SdXfer *x, const char *local, const char *remote_dir, GError **err) {
    char *base = g_path_get_basename(local);
    char *target = join_path(remote_dir, base);
    gboolean ok = TRUE;
    if (g_file_test(local, G_FILE_TEST_IS_DIR)) {
        GError *e2 = NULL;
        char *cmd = g_strdup_printf("MKD %s", target);
        ftp_quote(x, remote_dir, cmd, NULL, &e2);   /* already existing is fine */
        g_clear_error(&e2);
        g_free(cmd);
        GDir *d = g_dir_open(local, 0, NULL);
        const char *n;
        while (ok && d && (n = g_dir_read_name(d))) {
            char *child = g_build_filename(local, n, NULL);
            ok = ftp_upload_tree(x, child, target, err);
            g_free(child);
        }
        if (d) g_dir_close(d);
    } else {
        ok = ftp_put_file(x, local, target, err);
    }
    g_free(base); g_free(target);
    return ok;
}

/* ---- public operations ------------------------------------------------------------------------------------------------- */

char *sd_xfer_home(SdXfer *x, GError **err) {
    if (x->ftp) {
        CURL *c = ftp_handle(x, "/", TRUE);   /* FTP logins start in the user's home; report "/" as the browsable root */
        curl_easy_cleanup(c);
        return g_strdup("/");
    }
    gboolean used;
    char *cmds[] = {"pwd", NULL};
    char *out = try_sftp(x, cmds, 20, &used, err);
    if (!used) return ssh_home(x, err);
    if (!out) return NULL;
    char *p = strstr(out, "Remote working directory:");
    char *res = p ? g_strstrip(g_strdup(p + 25)) : NULL;
    if (!res) set_err(err, "could not read the remote home directory");
    g_free(out);
    return res;
}

GPtrArray *sd_xfer_list(SdXfer *x, const char *path, GError **err) {
    GPtrArray *r = NULL;
    if (x->ftp) r = ftp_list(x, path, err);
    else {
        gboolean used;
        char *q = sftp_quote(path), *cd = g_strdup_printf("cd %s", q);
        char *cmds[] = {cd, "ls -la", NULL};
        char *out = try_sftp(x, cmds, 60, &used, err);
        g_free(q); g_free(cd);
        if (!used) {
            char *sq = g_shell_quote(path), *cmd = g_strdup_printf("cd -- %s && LC_ALL=C ls -la", sq);
            out = ssh_run(x, cmd, 60, err);
            g_free(sq); g_free(cmd);
        }
        if (out) { r = sd_parse_ls(out); g_free(out); }
    }
    if (r) g_ptr_array_sort(r, entry_cmp);
    return r;
}

gboolean sd_xfer_is_dir(SdXfer *x, const char *path) {
    GError *e = NULL;
    gboolean ok;
    if (x->ftp) { GPtrArray *l = ftp_list(x, path, &e); ok = l != NULL; if (l) g_ptr_array_free(l, TRUE); }
    else {
        gboolean used;
        char *q = sftp_quote(path), *cd = g_strdup_printf("cd %s", q);
        char *cmds[] = {cd, NULL};
        char *out = try_sftp(x, cmds, 20, &used, &e);
        g_free(q); g_free(cd);
        if (used) { ok = out != NULL; g_free(out); }
        else { char *sq = g_shell_quote(path), *cmd = g_strdup_printf("test -d %s", sq); char *o = ssh_run(x, cmd, 20, &e); ok = o != NULL; g_free(o); g_free(sq); g_free(cmd); }
    }
    g_clear_error(&e);
    return ok;
}

gboolean sd_xfer_mkdir(SdXfer *x, const char *path, GError **err) {
    if (x->ftp) { char *dir = g_path_get_dirname(path), *cmd = g_strdup_printf("MKD %s", path); gboolean ok = ftp_quote(x, dir, cmd, NULL, err); g_free(dir); g_free(cmd); return ok; }
    gboolean used;
    char *q = sftp_quote(path), *c1 = g_strdup_printf("mkdir %s", q);
    char *cmds[] = {c1, NULL};
    char *out = try_sftp(x, cmds, 30, &used, err);
    g_free(q); g_free(c1);
    if (used) { g_free(out); return out != NULL; }
    char *sq = g_shell_quote(path), *cmd = g_strdup_printf("mkdir -- %s", sq);
    out = ssh_run(x, cmd, 30, err);
    g_free(sq); g_free(cmd); g_free(out);
    return out != NULL;
}

gboolean sd_xfer_rename(SdXfer *x, const char *from, const char *to, GError **err) {
    if (x->ftp) {
        char *dir = g_path_get_dirname(from), *a = g_strdup_printf("RNFR %s", from), *b = g_strdup_printf("RNTO %s", to);
        gboolean ok = ftp_quote(x, dir, a, b, err);
        g_free(dir); g_free(a); g_free(b);
        return ok;
    }
    gboolean used;
    char *qa = sftp_quote(from), *qb = sftp_quote(to), *c1 = g_strdup_printf("rename %s %s", qa, qb);
    char *cmds[] = {c1, NULL};
    char *out = try_sftp(x, cmds, 30, &used, err);
    g_free(qa); g_free(qb); g_free(c1);
    if (used) { g_free(out); return out != NULL; }
    char *sa = g_shell_quote(from), *sb = g_shell_quote(to), *cmd = g_strdup_printf("mv -- %s %s", sa, sb);
    out = ssh_run(x, cmd, 30, err);
    g_free(sa); g_free(sb); g_free(cmd); g_free(out);
    return out != NULL;
}

gboolean sd_xfer_remove(SdXfer *x, const char *path, gboolean is_dir, GError **err) {
    if (x->ftp) return ftp_remove_tree(x, path, is_dir, err);
    char *out;
    if (is_dir) {   /* sftp has no recursive delete */
        char *sq = g_shell_quote(path), *cmd = g_strdup_printf("rm -rf -- %s", sq);
        out = ssh_run(x, cmd, 600, err);
        g_free(sq); g_free(cmd);
    } else {
        gboolean used;
        char *q = sftp_quote(path), *c1 = g_strdup_printf("rm %s", q);
        char *cmds[] = {c1, NULL};
        out = try_sftp(x, cmds, 60, &used, err);
        g_free(q); g_free(c1);
        if (!used) { char *sq = g_shell_quote(path), *cmd = g_strdup_printf("rm -- %s", sq); out = ssh_run(x, cmd, 60, err); g_free(sq); g_free(cmd); }
    }
    g_free(out);
    return out != NULL;
}

/* run `sh -c script`; used for tar pipelines */
static gboolean run_script(const char *script, GError **err) {
    char *bash = g_find_program_in_path("bash");
    const char *shell = bash ? "bash" : "sh";
    g_free(bash);
    char *full = g_strdup_printf("%s%s", g_str_equal(shell, "bash") ? "set -o pipefail; " : "", script);
    char *argv[] = {(char *)shell, "-c", full, NULL};
    Run r;
    gboolean ok = run_cmd(argv, NULL, 0, &r) && r.status == 0;
    if (!ok) set_err(err, "%s", r.err && *g_strstrip(r.err) ? r.err : "transfer failed");
    run_free(&r);
    g_free(full);
    return ok;
}

static char *ssh_prefix(SdXfer *x) {
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup("ssh"));
    mux_args(x, a, "-p");
    g_ptr_array_add(a, g_strdup(x->dest));
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < a->len; i++) { char *q = g_shell_quote(a->pdata[i]); g_string_append_printf(s, i ? " %s" : "%s", q); g_free(q); }
    g_ptr_array_free(a, TRUE);
    return g_string_free(s, FALSE);
}

static gboolean scp_run(SdXfer *x, char **tail, GError **err) {
    for (int legacy = 1; legacy >= 0; legacy--) {
        GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(a, g_strdup("scp"));
        mux_args(x, a, "-P");
        if (legacy) g_ptr_array_add(a, g_strdup("-O"));   /* classic protocol: works without an sftp subsystem */
        for (char **t = tail; *t; t++) g_ptr_array_add(a, g_strdup(*t));
        char **argv = finish(a);
        Run r;
        gboolean started = run_cmd(argv, NULL, 0, &r);
        g_strfreev(argv);
        if (!started) { set_err(err, "the scp command is not installed"); return FALSE; }
        gboolean ok = r.status == 0;
        gboolean retry = legacy && !ok && (strcasestr(r.err, "unknown option") || strcasestr(r.err, "illegal option") || strcasestr(r.err, "usage:"));
        if (!ok && !retry) set_err(err, "%s", *g_strstrip(r.err) ? r.err : "scp failed");
        run_free(&r);
        if (ok) return TRUE;
        if (!retry) return FALSE;
    }
    return FALSE;
}

static gboolean fallback_transfer(SdXfer *x, gboolean (*scp)(SdXfer *, gpointer, GError **),
                                  gboolean (*stream)(SdXfer *, gpointer, GError **), gpointer arg, GError **err) {
    GString *errors = g_string_new(NULL);
    for (guint i = 0; i < x->xfer_order->len; i++) {
        const char *m = x->xfer_order->pdata[i];
        GError *e = NULL;
        gboolean ok = g_str_equal(m, "scp") ? scp(x, arg, &e) : stream(x, arg, &e);
        if (ok) { g_string_free(errors, TRUE); return TRUE; }
        g_string_append_printf(errors, "%s%s: %s", errors->len ? "; " : "", m, e ? e->message : "failed");
        g_clear_error(&e);
    }
    set_err(err, "%s", errors->str);
    g_string_free(errors, TRUE);
    return FALSE;
}

typedef struct { const char *remote, *local_dir; gboolean is_dir; } DownArgs;
typedef struct { const char *local, *remote_dir; } UpArgs;

static gboolean down_scp(SdXfer *x, gpointer p, GError **err) {
    DownArgs *a = p;
    char *qrem = scp_quote(a->remote);
    char *src = g_strdup_printf("%s:%s", x->dest, qrem);
    g_free(qrem);
    char *dst = g_strdup_printf("%s/", a->local_dir);
    char *tail_d[] = {"-r", src, dst, NULL}, *tail_f[] = {src, dst, NULL};
    gboolean ok = scp_run(x, a->is_dir ? tail_d : tail_f, err);
    g_free(src); g_free(dst);
    return ok;
}

static gboolean down_stream(SdXfer *x, gpointer p, GError **err) {
    DownArgs *a = p;
    char *ssh = ssh_prefix(x), *parent = g_path_get_dirname(a->remote), *name = g_path_get_basename(a->remote);
    char *qp = g_shell_quote(parent), *qn = g_shell_quote(name), *ql = g_shell_quote(a->local_dir);
    char *remote_cmd, *script;
    if (a->is_dir) {
        remote_cmd = g_strdup_printf("tar -C %s -cf - %s", qp, qn);
        char *qr = g_shell_quote(remote_cmd);
        script = g_strdup_printf("%s %s | tar -C %s -xf -", ssh, qr, ql);
        g_free(qr);
    } else {
        char *full = g_strdup(a->remote), *qf = g_shell_quote(full);
        remote_cmd = g_strdup_printf("cat -- %s", qf);
        char *qr = g_shell_quote(remote_cmd), *dst = g_build_filename(a->local_dir, name, NULL), *qd = g_shell_quote(dst);
        script = g_strdup_printf("%s %s > %s", ssh, qr, qd);
        g_free(qr); g_free(dst); g_free(qd); g_free(full); g_free(qf);
    }
    gboolean ok = run_script(script, err);
    g_free(ssh); g_free(parent); g_free(name); g_free(qp); g_free(qn); g_free(ql); g_free(remote_cmd); g_free(script);
    return ok;
}

static gboolean up_scp(SdXfer *x, gpointer p, GError **err) {
    UpArgs *a = p;
    char *rd = g_str_has_suffix(a->remote_dir, "/") ? g_strdup(a->remote_dir) : g_strconcat(a->remote_dir, "/", NULL);
    char *q = scp_quote(rd), *dst = g_strdup_printf("%s:%s", x->dest, q);
    gboolean dir = g_file_test(a->local, G_FILE_TEST_IS_DIR);
    char *tail_d[] = {"-r", (char *)a->local, dst, NULL}, *tail_f[] = {(char *)a->local, dst, NULL};
    gboolean ok = scp_run(x, dir ? tail_d : tail_f, err);
    g_free(rd); g_free(q); g_free(dst);
    return ok;
}

static gboolean up_stream(SdXfer *x, gpointer p, GError **err) {
    UpArgs *a = p;
    char *ssh = ssh_prefix(x), *name = g_path_get_basename(a->local), *parent = g_path_get_dirname(a->local);
    char *script;
    if (g_file_test(a->local, G_FILE_TEST_IS_DIR)) {
        char *qp = g_shell_quote(parent), *qn = g_shell_quote(name), *rd = g_shell_quote(a->remote_dir);
        char *rc = g_strdup_printf("tar -C %s -xf -", rd), *qrc = g_shell_quote(rc);
        script = g_strdup_printf("tar -C %s -cf - %s | %s %s", qp, qn, ssh, qrc);
        g_free(qp); g_free(qn); g_free(rd); g_free(rc); g_free(qrc);
    } else {
        char *target = join_path(a->remote_dir, name), *qt = g_shell_quote(target), *rc = g_strdup_printf("cat > %s", qt);
        char *qrc = g_shell_quote(rc), *ql = g_shell_quote(a->local);
        script = g_strdup_printf("%s %s < %s", ssh, qrc, ql);
        g_free(target); g_free(qt); g_free(rc); g_free(qrc); g_free(ql);
    }
    gboolean ok = run_script(script, err);
    g_free(ssh); g_free(name); g_free(parent); g_free(script);
    return ok;
}

gboolean sd_xfer_download(SdXfer *x, const char *remote, const char *local_dir, gboolean is_dir, GError **err) {
    if (x->ftp) return ftp_download_tree(x, remote, local_dir, is_dir, err);
    gboolean used;
    char *qr = sftp_quote(remote), *ql = sftp_quote(local_dir), *c1 = g_strdup_printf("get %s%s %s", is_dir ? "-r " : "", qr, ql);
    char *cmds[] = {c1, NULL};
    char *out = try_sftp(x, cmds, 0, &used, err);
    g_free(qr); g_free(ql); g_free(c1);
    if (used) { g_free(out); return out != NULL; }
    DownArgs a = {remote, local_dir, is_dir};
    return fallback_transfer(x, down_scp, down_stream, &a, err);
}

gboolean sd_xfer_upload(SdXfer *x, const char *local, const char *remote_dir, GError **err) {
    if (x->ftp) return ftp_upload_tree(x, local, remote_dir, err);
    gboolean used;
    gboolean dir = g_file_test(local, G_FILE_TEST_IS_DIR);
    char *ql = sftp_quote(local), *qr = sftp_quote(remote_dir), *c1 = g_strdup_printf("put %s%s %s", dir ? "-r " : "", ql, qr);
    char *cmds[] = {c1, NULL};
    char *out = try_sftp(x, cmds, 0, &used, err);
    g_free(qr); g_free(ql); g_free(c1);
    if (used) { g_free(out); return out != NULL; }
    UpArgs a = {local, remote_dir};
    return fallback_transfer(x, up_scp, up_stream, &a, err);
}

char *sd_xfer_terminal_cwd(SdXfer *x) {
    if (x->ftp) return NULL;
    /* the exec'd command and the login shell are siblings under one sshd process: $PPID finds the shell */
    const char *script = "p=$(ps -o pid=,tty= --ppid $PPID 2>/dev/null | awk '$2!=\"?\"{print $1; exit}'); "
                         "[ -n \"$p\" ] && readlink /proc/$p/cwd";
    char *o = ssh_run(x, script, 5, NULL);
    if (o) g_strstrip(o);
    if (o && !*o) { g_free(o); o = NULL; }
    return o;
}
