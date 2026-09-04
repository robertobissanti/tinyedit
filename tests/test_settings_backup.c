#define _DEFAULT_SOURCE

#include "backup.h"
#include "settings.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    exit(1);
}

int main(void) {
    char home[] = "/tmp/tinyedit-settings-XXXXXX";
    if (!mkdtemp(home)) fail("mkdtemp");
    if (setenv("HOME", home, 1) != 0) fail("setenv");

    char config[1024];
    snprintf(config, sizeof(config), "%s/.tinyeditrc", home);
    FILE *fp = fopen(config, "w");
    if (!fp) fail("open test config");
    fputs("show_line_numbers = malformed\n", fp);
    fputs("tab_stop = not-a-number\n", fp);
    fputs("backup_interval = -20\n", fp);
    fputs("auto_indent = false\n", fp);
    fputs("color_gutter = cyan-dark\n", fp);
    if (fclose(fp) != 0) fail("close test config");

    struct editorSettings settings;
    settingsLoad(&settings);
    if (settings.show_line_numbers != 1) fail("malformed bool changed its default");
    if (settings.tab_stop != 4) fail("malformed integer changed its default");
    if (settings.backup_interval != 0) fail("integer range clamp");
    if (settings.auto_indent != 0) fail("valid bool parse");
    if (settings.color_gutter != COLOR_CYAN_DARK) fail("valid enum parse");
    if (!settingsSave(&settings)) fail("settingsSave");

    char filename[1024];
    snprintf(filename, sizeof(filename), "%s/document.txt", home);
    fp = fopen(filename, "w");
    if (!fp || fclose(fp) != 0) fail("create backup target");
    const char content[] = "caffè\nsecond line\n";
    if (!backupWrite(filename, content, sizeof(content) - 1)) fail("backupWrite");
    if (!backupExists(filename)) fail("backupExists");
    size_t len = 0;
    char *restored = backupRead(filename, &len);
    if (!restored || len != sizeof(content) - 1 || memcmp(restored, content, len) != 0)
        fail("backup round trip");
    free(restored);
    backupRemove(filename);
    if (backupExists(filename)) fail("backupRemove");

    unlink(filename);
    unlink(config);
    puts("settings/backup tests: ok");
    return 0;
}
