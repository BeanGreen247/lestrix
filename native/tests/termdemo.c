/* Minimal host for the terminal widget: shows one shell, optionally runs a flood benchmark. */
#include <gtk/gtk.h>
#include <string.h>

#include "../src/term.h"

static gint64 t0;
static gboolean bench;
static const char *demo_cmd;
static SdTerm *term;
static GtkApplication *app_global;

static gboolean poll_done(gpointer data) {
    (void)data;
    /* the marker line only appears when the whole flood has been consumed */
    extern gboolean sd_term_screen_contains(SdTerm *, const char *);
    if (sd_term_screen_contains(term, "FLOOD_DONE_42")) {
        g_print("flood finished in %.2fs (%s renderer)\n", (g_get_monotonic_time() - t0) / 1e6,
                g_getenv("LESTRIX_RENDERER") ? g_getenv("LESTRIX_RENDERER") : "default");
        g_application_quit(G_APPLICATION(app_global));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean start_flood(gpointer data) {
    (void)data;
    t0 = g_get_monotonic_time();
    const char *cmd = "seq 1 300000; yes 'the quick brown fox jumps over the lazy dog 0123456789' | head -n 100000; echo FLOOD_DONE_$((6*7))\r";
    sd_term_send(term, cmd, strlen(cmd));
    g_timeout_add(20, poll_done, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean send_demo(gpointer d) { (void)d; sd_term_send(term, demo_cmd, strlen(demo_cmd)); return G_SOURCE_REMOVE; }

static void activate(GtkApplication *app, gpointer data) {
    (void)data;
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(win), 1400, 900);
    char *argv[] = {"bash", "--norc", "--noprofile", NULL};
    term = sd_term_new(argv, NULL);
    sd_term_set_font(term, "monospace", 11);
    gtk_window_set_child(GTK_WINDOW(win), GTK_WIDGET(term));
    gtk_window_present(GTK_WINDOW(win));
    if (bench) g_timeout_add(1500, start_flood, NULL);
    if (demo_cmd) g_timeout_add(1200, send_demo, NULL);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench")) { bench = TRUE; argv[i] = (char *)"--"; }
        else if (!strcmp(argv[i], "--cmd") && i + 1 < argc) { demo_cmd = argv[i + 1]; argv[i] = argv[i + 1] = (char *)"--"; i++; }
    }
    GtkApplication *app = gtk_application_new("org.example.termdemo", G_APPLICATION_NON_UNIQUE);
    app_global = app;
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return rc;
}
