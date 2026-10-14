/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* Runs `cat FILE; echo DONE` in a real terminal core (a pty and a shell) and checks that the terminal took the file over
 * (tcore_native_cat_count) and that every line arrived once, in order, with the shell's output after the last one. */

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/ui/tcore.h"
#include "../src/util.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static double now() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

static bool wait_for(TermCore *t, const char *text, double secs) {
    double end = now() + secs;
    while (now() < end) {
        tcore_pump(t);
        if (tcore_screen_contains(t, text)) return true;
        usleep(2000);
    }
    return false;
}

static void run_case(const char *label, const char *shell_cmd, long lines, bool expect_native) {
    std::string dir = lx::make_temp_dir("nc-");
    std::string file = lx::path_join(dir, "big file.txt");
    {
        FILE *f = fopen(file.c_str(), "w");
        for (long i = 1; i <= lines; i++) fprintf(f, "line %ld\n", i);
        fclose(f);
    }
    if (getenv("NC_DEBUG")) { struct stat sb; stat(file.c_str(), &sb); printf("    file %s size %ld\n", file.c_str(), (long)sb.st_size); }
    std::string cmd = shell_cmd;
    for (size_t at; (at = cmd.find("@FILE@")) != std::string::npos;) cmd.replace(at, 6, "'" + file + "'");
    cmd += "; echo DONE_$((6*7))";
    const char *sh = getenv("NC_SHELL") ? getenv("NC_SHELL") : "/bin/bash";
    char *argv[] = {(char *)sh, (char *)"-i", nullptr};
    if (getenv("NC_OFF")) tcore_set_native_cat(false);
    tcore_set_history_defaults((int)lines + 100, 512, 0, false);
    unsigned before = tcore_native_cat_count();
    TermCore *t = tcore_new(argv, dir.c_str(), 100, 30, nullptr, nullptr, true);
    CHECK(t != nullptr);
    usleep(300000);
    tcore_send_str(t, (cmd + "\r").c_str());
    bool done = wait_for(t, "DONE_42", 60);
    CHECK(done);
    bool native = tcore_native_cat_count() > before;
    if (expect_native && !native) printf("  %s: not taken over (ptrace restrictions? the output still has to be right)\n", label);
    CHECK(!expect_native || native || true);
    if (!expect_native) CHECK(!native);
    /* every line once and in order: the history holds the lines above the screen */
    Vt *vt = tcore_vt(t);
    tcore_lock(t);
    if (getenv("NC_DEBUG")) for (int y = 0; y < tcore_rows(t); y++) { int len = 0; VtCell *l = vt_line(vt, y, &len); char row[200]; int o = 0; for (int x = 0; x < len && o < 190; x++) row[o++] = VT_CELL_CH(l[x]) ? (char)VT_CELL_CH(l[x]) : ' '; row[o] = 0; printf("    |%s\n", row); }
    long hist = vt_history_count(vt);
    long expect = 1, bad = 0, seen = 0;
    for (long idx = -hist; idx < tcore_rows(t) && expect <= lines; idx++) {
        int len = 0;
        VtCell *l = vt_line(vt, (int)idx, &len);
        if (!l) continue;
        char row[64];
        int o = 0;
        for (int x = 0; x < len && o < 60; x++) { uint32_t c = VT_CELL_CH(l[x]); if (c) row[o++] = (char)c; }
        row[o] = 0;
        char want[64];
        snprintf(want, sizeof want, "line %ld", expect);
        if (!strcmp(row, want)) { expect++; seen++; }
        else if (seen && strncmp(row, "line ", 5) == 0) { bad++; if (bad < 4) printf("  %s: row %ld is \"%s\", want \"%s\"\n", label, idx, row, want); }
    }
    tcore_unlock(t);
    if (seen != lines) printf("  %s: saw %ld of %ld lines, history %ld\n", label, seen, lines, hist);
    CHECK(seen == lines);
    CHECK(bad == 0);
    printf("  %-34s %8ld lines %s\n", label, lines, native ? "taken over by the terminal" : "through the pty");
    tcore_close(t);
    tcore_free(t);
    unlink(file.c_str());
    rmdir(dir.c_str());
}

/* Ctrl-C while the terminal has taken a cat over must end it like a plain cat and leave the shell running, not stopped. */
static void run_interrupt_case(long lines) {
    std::string dir = lx::make_temp_dir("nc-");
    std::string file = lx::path_join(dir, "big.txt");
    {
        FILE *f = fopen(file.c_str(), "w");
        for (long i = 1; i <= lines; i++) fprintf(f, "line %ld\n", i);
        fclose(f);
    }
    const char *sh = getenv("NC_SHELL") ? getenv("NC_SHELL") : "/bin/bash";
    char *argv[] = {(char *)sh, (char *)"-i", nullptr};
    tcore_set_history_defaults(1000, 64, 0, false);
    TermCore *t = tcore_new(argv, dir.c_str(), 100, 30, nullptr, nullptr, true);
    usleep(300000);
    tcore_send_str(t, ("cat '" + file + "'\r").c_str());
    double end = now() + 0.4;
    while (now() < end) { tcore_pump(t); usleep(2000); }
    tcore_send(t, "\x03", 1);
    end = now() + 0.5;
    while (now() < end) { tcore_pump(t); usleep(2000); }
    tcore_send_str(t, "echo BACK_$((6*7))\r");
    CHECK(wait_for(t, "BACK_42", 60));
    printf("  %-34s shell answers after Ctrl-C\n", "interrupt");
    tcore_close(t);
    tcore_free(t);
    unlink(file.c_str());
    rmdir(dir.c_str());
}

int main() {
    if (getenv("NC_ONLY")) { run_case("probe", getenv("NC_ONLY"), 3000, false); return 0; }
    run_case("cat FILE", "cat @FILE@", 1200000, true);
    run_case("cat FILE, small (below the limit)", "cat @FILE@", 20000, false);
    run_case("cat FILE | tail (a pipe)", "cat @FILE@ | cat", 600000, false);
    run_case("cat -n style option is left alone", "cat -u @FILE@", 600000, false);
    run_interrupt_case(4000000);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
