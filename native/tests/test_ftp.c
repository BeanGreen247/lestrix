/* FTP backend test against tests/ftp_server.py (stdlib server, no extra packages). */
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/xfer.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; g_print("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    char *root = g_dir_make_tmp("sdftp-XXXXXX", NULL);
    char *argv[] = {"python3", "tests/ftp_server.py", root, NULL};
    GPid pid;
    gint out_fd;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL, &out_fd, NULL, NULL)) {
        g_print("  (skipping ftp: python3 not available)\n");
        return 0;
    }
    char buf[32] = {0};
    ssize_t n = read(out_fd, buf, sizeof buf - 1);
    int port = n > 0 ? atoi(buf) : 0;
    CHECK(port > 0);

    SdConn *c = sd_conn_new();
    sd_conn_set(&c->host, "127.0.0.1");
    sd_conn_set(&c->user, "tester");
    sd_conn_set(&c->protocol, "ftp");
    c->port = port;
    SdXfer *x = sd_xfer_new_ftp(c, "secret");
    GError *err = NULL;

    char *tmpd = g_dir_make_tmp("sdftpc-XXXXXX", NULL);
    char *f = g_build_filename(tmpd, "up file.txt", NULL), *d = g_build_filename(tmpd, "updir", NULL), *di = g_build_filename(d, "inner.txt", NULL);
    g_file_set_contents(f, "hi ftp", -1, NULL);
    g_mkdir_with_parents(d, 0755);
    g_file_set_contents(di, "deep ftp", -1, NULL);

    char *home = sd_xfer_home(x, &err);
    CHECK(home && g_str_equal(home, "/"));
    CHECK(sd_xfer_mkdir(x, "/work", &err));
    CHECK(sd_xfer_upload(x, f, "/work", &err));
    CHECK(sd_xfer_upload(x, d, "/work", &err));
    GPtrArray *l = sd_xfer_list(x, "/work", &err);
    CHECK(l && l->len == 2 && g_str_equal(((SdEntry *)l->pdata[0])->name, "updir") && ((SdEntry *)l->pdata[0])->is_dir);
    CHECK(l && g_str_equal(((SdEntry *)l->pdata[1])->name, "up file.txt") && ((SdEntry *)l->pdata[1])->size == 6);
    if (l) g_ptr_array_free(l, TRUE);

    char *out = g_build_filename(tmpd, "dl", NULL);
    g_mkdir_with_parents(out, 0755);
    CHECK(sd_xfer_download(x, "/work/up file.txt", out, FALSE, &err));
    CHECK(sd_xfer_download(x, "/work/updir", out, TRUE, &err));
    char *got = NULL;
    char *p1 = g_build_filename(out, "up file.txt", NULL), *p2 = g_build_filename(out, "updir", "inner.txt", NULL);
    CHECK(g_file_get_contents(p1, &got, NULL, NULL) && g_str_equal(got, "hi ftp")); g_free(got); got = NULL;
    CHECK(g_file_get_contents(p2, &got, NULL, NULL) && g_str_equal(got, "deep ftp")); g_free(got);

    CHECK(sd_xfer_rename(x, "/work/up file.txt", "/work/renamed.txt", &err));
    CHECK(sd_xfer_is_dir(x, "/work") && g_file_test(g_build_filename(root, "work", "renamed.txt", NULL), G_FILE_TEST_EXISTS));
    CHECK(sd_xfer_remove(x, "/work", TRUE, &err));
    CHECK(!g_file_test(g_build_filename(root, "work", NULL), G_FILE_TEST_EXISTS));
    char *label = sd_xfer_label(x);
    CHECK(g_str_equal(label, "FTP"));
    if (err) { g_print("  last error: %s\n", err->message); g_clear_error(&err); }

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    g_print("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
