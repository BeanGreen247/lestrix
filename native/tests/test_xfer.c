/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/xfer.h"

static int failures, checks, skipped;
#define CHECK(c) do { checks++; if (!(c)) { failures++; g_print("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *OPTS[] = {"-o", "UserKnownHostsFile=/dev/null", "-o", "StrictHostKeyChecking=no", "-o", "LogLevel=ERROR", NULL};

static void test_parse_ls(void) {
    GPtrArray *a = sd_parse_ls(
        "sftp> ls -la\n"
        "drwxr-xr-x    ? bob      bob          4096 Oct  3 12:01 My Docs\n"
        "-rw-r--r--    1 bob      bob           120 Jan  5  2025 notes.txt\n"
        "lrwxrwxrwx    1 bob      bob             4 Jan  5 10:00 link -> docs\n"
        "drwxr-xr-x    2 bob      bob          4096 Oct  3 12:01 .\n"
        "drwxr-xr-x    9 bob      bob          4096 Oct  3 12:01 ..\n"
        "total 12\n");
    CHECK(a->len == 3);
    SdEntry *d = a->pdata[0], *f = a->pdata[1], *l = a->pdata[2];
    CHECK(g_str_equal(d->name, "My Docs") && d->is_dir);
    CHECK(g_str_equal(f->name, "notes.txt") && f->size == 120 && !f->is_dir);
    CHECK(g_str_equal(l->name, "link") && l->is_link);
    g_ptr_array_free(a, TRUE);
}

static gboolean localhost_ok(void) {
    char *argv[] = {"ssh", (char *)OPTS[0], (char *)OPTS[1], (char *)OPTS[2], (char *)OPTS[3], (char *)OPTS[4], (char *)OPTS[5],
                    "-o", "BatchMode=yes", "-o", "ConnectTimeout=3", "localhost", "true", NULL};
    int st = 0;
    return g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &st, NULL) && st == 0;
}

static void roundtrip(const char *mode, const char *order) {
    char *cp = sd_xfer_new_control_path();
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    const char *base[] = {"ssh", NULL};
    for (int i = 0; base[i]; i++) g_ptr_array_add(a, g_strdup(base[i]));
    for (int i = 0; OPTS[i]; i++) g_ptr_array_add(a, g_strdup(OPTS[i]));
    const char *rest[] = {"-N", "-o", "ControlMaster=yes", "-o", NULL};
    for (int i = 0; rest[i]; i++) g_ptr_array_add(a, g_strdup(rest[i]));
    g_ptr_array_add(a, g_strdup_printf("ControlPath=%s", cp));
    g_ptr_array_add(a, g_strdup("localhost"));
    g_ptr_array_add(a, NULL);
    GPid pid;
    CHECK(g_spawn_async(NULL, (char **)a->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL));

    SdConn *conn = sd_conn_new();
    sd_conn_set(&conn->host, "localhost");
    SdXfer *x = sd_xfer_new_ssh(conn, cp);
    for (int i = 0; i < 60 && !sd_xfer_ready(x); i++) g_usleep(100000);
    CHECK(sd_xfer_ready(x));
    sd_xfer_force_mode(x, mode, order);

    GError *err = NULL;
    char *home = sd_xfer_home(x, &err);
    CHECK(home && home[0] == '/');
    char *work = g_strdup_printf("%s/.lestrix-ctest dir", home);
    CHECK(sd_xfer_mkdir(x, work, &err));
    char *tmpd = g_dir_make_tmp("sdx-XXXXXX", NULL);
    char *f = g_build_filename(tmpd, "up file.txt", NULL), *d = g_build_filename(tmpd, "updir", NULL), *di = g_build_filename(d, "inner.txt", NULL);
    g_file_set_contents(f, "hi", -1, NULL);
    g_mkdir_with_parents(d, 0755);
    g_file_set_contents(di, "deep", -1, NULL);
    CHECK(sd_xfer_upload(x, f, work, &err));
    CHECK(sd_xfer_upload(x, d, work, &err));
    GPtrArray *l = sd_xfer_list(x, work, &err);
    CHECK(l && l->len == 2 && g_str_equal(((SdEntry *)l->pdata[0])->name, "updir") && g_str_equal(((SdEntry *)l->pdata[1])->name, "up file.txt"));
    if (l) g_ptr_array_free(l, TRUE);
    char *out = g_build_filename(tmpd, "dl", NULL);
    g_mkdir_with_parents(out, 0755);
    char *rf = g_strdup_printf("%s/up file.txt", work), *rd = g_strdup_printf("%s/updir", work);
    CHECK(sd_xfer_download(x, rf, out, FALSE, &err));
    CHECK(sd_xfer_download(x, rd, out, TRUE, &err));
    char *got = NULL;
    char *p1 = g_build_filename(out, "up file.txt", NULL), *p2 = g_build_filename(out, "updir", "inner.txt", NULL);
    CHECK(g_file_get_contents(p1, &got, NULL, NULL) && g_str_equal(got, "hi")); g_free(got); got = NULL;
    CHECK(g_file_get_contents(p2, &got, NULL, NULL) && g_str_equal(got, "deep")); g_free(got);
    char *rn = g_strdup_printf("%s/renamed.txt", work);
    CHECK(sd_xfer_rename(x, rf, rn, &err));
    CHECK(sd_xfer_is_dir(x, work) && !sd_xfer_is_dir(x, rn));
    CHECK(sd_xfer_remove(x, rn, FALSE, &err));
    CHECK(sd_xfer_remove(x, work, TRUE, &err));
    CHECK(!sd_xfer_is_dir(x, work));
    if (err) { g_print("  last error (%s): %s\n", mode, err->message); g_clear_error(&err); }
    char *label = sd_xfer_label(x);
    g_print("  %-22s ok, label \"%s\"\n", order ? order : "sftp", label);
    g_free(label);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    sd_xfer_remove_control_path(cp);
    sd_xfer_free(x); sd_conn_free(conn);
    g_free(cp); g_free(home); g_free(work); g_free(tmpd); g_free(f); g_free(d); g_free(di); g_free(out); g_free(rf); g_free(rd); g_free(rn); g_free(p1); g_free(p2);
    g_ptr_array_free(a, TRUE);
}

int main(void) {
    test_parse_ls();
    if (localhost_ok()) {
        roundtrip("sftp", NULL);
        roundtrip("ssh", "scp");
        roundtrip("ssh", "ssh");
    } else {
        skipped = 1;
        g_print("  (skipping ssh backends: passwordless ssh to localhost not available)\n");
    }
    g_print("%d checks, %d failures%s\n", checks, failures, skipped ? ", ssh tests skipped" : "");
    return failures ? 1 : 0;
}
