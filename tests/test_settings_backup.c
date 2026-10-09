#define _DEFAULT_SOURCE

#include "backup.h"
#include "settings.h"

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    exit(1);
}

int main(void) {
    uint8_t found_mac_command_keys = 0;
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        if (strcmp(settingDescriptors[i].key, "mac_command_keys") == 0) {
            found_mac_command_keys = 1;
#ifdef __APPLE__
            if (!strstr(settingDescriptors[i].label, "experimental"))
                fail("Ghostty option is not marked experimental");
#endif
        }
    }
#ifdef __APPLE__
    if (!found_mac_command_keys) fail("Ghostty option missing on macOS");
#else
    if (found_mac_command_keys) fail("Ghostty option present off macOS");
#endif

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
    fputs("scrolloff = 999\n", fp);
    fputs("undo_memory_mb = 32\n", fp);
    fputs("auto_indent = false\n", fp);
    fputs("color_gutter = cyan-dark\n", fp);
    fputs("color_syntax_json_key = red-dark\ncolor_syntax_bracket = magenta-light\n", fp);
    if (fclose(fp) != 0) fail("close test config");

    struct editorSettings settings;
    settingsLoad(&settings);
    if (settings.show_line_numbers != 1) fail("malformed bool changed its default");
    if (settings.tab_stop != 4) fail("malformed integer changed its default");
    if (settings.backup_interval != 0) fail("integer range clamp");
    if (settings.scrolloff != 20) fail("scrolloff range clamp");
    if (settings.auto_indent != 0) fail("valid bool parse");
    if (settings.color_gutter != COLOR_CYAN_DARK) fail("valid enum parse");
    if (settings.undo_memory_mb != 32) fail("undo memory budget parse");
    if (settings.color_syntax_json_key != COLOR_RED_DARK ||
        settings.color_syntax_bracket != COLOR_MAGENTA_LIGHT) fail("new color parsing");
    if (!settingsSave(&settings)) fail("settingsSave");
    settingsLoad(&settings);
    if (settings.undo_memory_mb != 32) fail("undo memory budget persistence");
    if (settings.color_syntax_json_key != COLOR_RED_DARK ||
        settings.color_syntax_bracket != COLOR_MAGENTA_LIGHT) fail("new color persistence");

    if (settings.color_mode != COLOR_MODE_ANSI || settings.color_gutter != COLOR_CYAN_DARK)
        fail("old configuration keeps ANSI mode and colors");
    int32_t rgb = 123;
    const char *invalid[] = {"#fff", "#1234567", "#12345G", "123456", "#-12345", "#123456 junk", ""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (settingsParseRgb(invalid[i], &rgb) || rgb != 123) fail("invalid RGB mutates output");
    }
    if (!settingsParseRgb("#aB01fF", &rgb) || rgb != 0xab01ff) fail("RGB mixed case");
    char formatted[17];
    settingsFormatRgb(rgb, formatted, sizeof(formatted));
    if (strcmp(formatted, "#AB01FF")) fail("RGB canonical spelling");
    fp = fopen(config, "w");
    if (!fp) fail("open RGB config");
    fputs("color_mode = rgb\nrgb_output = truecolor\n  rgb_background = #123456 # comment\n"
        "rgb_syntax_keyword = #ABCDEF\nrgb_gutter = #bad\n"
        "rgb_markdown_heading_background = #112233\n"
        "rgb_syntax_normal = terminal-default\ncolor_gutter = cyan\n", fp);
    if (fclose(fp)) fail("close RGB config");
    settingsLoad(&settings);
    if (settings.color_mode != COLOR_MODE_RGB || settings.rgb_color_background != 0x123456 ||
        settings.rgb_markdown_heading_background != 0x112233 ||
        settings.rgb_color_syntax_keyword != 0xabcdef || settings.rgb_color_gutter != 0x808080 ||
        settings.rgb_color_syntax_normal != RGB_TERMINAL_DEFAULT || settings.color_gutter != COLOR_CYAN_LIGHT)
        fail("RGB configuration parsing, comments, invalid values and legacy names");
    if (strcmp(ansiBgColorCode(settingsColor(&settings, color_background)), "\x1b[48;2;18;52;86m") ||
        strcmp(ansiColorCode(settingsColor(&settings, color_syntax_keyword)), "\x1b[38;2;171;205;239m"))
        fail("RGB exact foreground/background escapes");
    struct editorSettings expected = settings;
    if (!settingsSave(&settings)) fail("save RGB");
    settingsLoad(&settings);
    if (memcmp(&settings, &expected, sizeof(settings))) fail("RGB and ANSI palettes round trip independently");
    settings.rgb_output = RGB_OUTPUT_ANSI_FALLBACK;
    int32_t fallback = settingsColor(&settings, color_background);
    if (fallback < 0 || fallback >= SETTING_HUE_COLOR_COUNT || settingColorIsDim(fallback))
        fail("manual fallback resolves to base ANSI color");
    settings.color_mode = COLOR_MODE_ANSI;
    if (settingsColor(&settings, color_gutter) != COLOR_CYAN_LIGHT) fail("switch restores ANSI palette");

    char scheme_error[128];
    struct editorSettings scheme = settings, before_scheme = settings;
    scheme.rgb_output = RGB_OUTPUT_ANSI_FALLBACK;
    if (!settingsLoadColorScheme("colorschemes/one-dark.conf", &scheme, scheme_error, sizeof(scheme_error)))
        fail("complete One scheme rejected");
    if (scheme.rgb_color_background != 0x282c34 || scheme.color_mode != COLOR_MODE_RGB ||
        scheme.tab_stop != before_scheme.tab_stop || scheme.rgb_output != RGB_OUTPUT_ANSI_FALLBACK ||
        scheme.color_gutter != before_scheme.color_gutter) fail("scheme changed unrelated settings or ANSI palette");
    if (!settingsLoadColorScheme("colorschemes/catppuccin-mocha.conf", &scheme, scheme_error, sizeof(scheme_error)) ||
        scheme.rgb_color_background != 0x1e1e2e) fail("Mocha scheme rejected");
    before_scheme = scheme;
    const char *invalid_schemes[] = {
        "color_mode = rgb\nrgb_background = #112233\n",
        "color_mode = ansi\ncolor_gutter = nonsense\n",
        "color_mode = rgb\ntab_stop = 8\n",
        "color_mode = rgb\ncolor_mode = rgb\n",
        "color_mode = rgb\nrgb_background = #gggggg\n"
    };
    for (size_t i = 0; i < sizeof(invalid_schemes) / sizeof(invalid_schemes[0]); i++) {
        fp = fopen(config, "w");
        if (!fp) fail("open invalid preset");
        fputs(invalid_schemes[i], fp); fclose(fp);
        if (settingsLoadColorScheme(config, &scheme, scheme_error, sizeof(scheme_error)) ||
            memcmp(&scheme, &before_scheme, sizeof(scheme)) || !scheme_error[0])
            fail("invalid preset modified draft");
    }
    fp = fopen(config, "w");
    if (!fp) fail("open ANSI scheme");
    fputs("color_mode = ansi\nmarkdown_heading_reverse = true\n", fp);
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        const struct settingDescriptor *d = &settingDescriptors[i];
        if (d->type == SETTING_ENUM && !strncmp(d->key, "color_", 6) && strcmp(d->key, "color_mode"))
            fprintf(fp, "%s = blue-light\n", d->key);
    }
    fclose(fp);
    if (!settingsLoadColorScheme(config, &scheme, scheme_error, sizeof(scheme_error)) ||
        scheme.color_mode != COLOR_MODE_ANSI || scheme.color_gutter != COLOR_BLUE_LIGHT ||
        !scheme.markdown_heading_reverse || scheme.rgb_color_background != before_scheme.rgb_color_background)
        fail("ANSI preset or independent palette preservation");

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

    /* Keep the backup directory under the short temporary HOME, but make the
     * edited file's resolved path longer than the old 1024-byte buffers. */
    char longdir[2048];
    snprintf(longdir, sizeof(longdir), "%s/long-path", home);
    if (mkdir(longdir, 0700) != 0) fail("create long path root");
    uint8_t long_path_available = 1;
    for (int32_t i = 0; i < 12; i++) {
        size_t used = strlen(longdir);
        if (used + 91 >= sizeof(longdir)) fail("long path buffer");
        longdir[used] = '/';
        memset(longdir + used + 1, 'a', 90);
        longdir[used + 91] = '\0';
        if (mkdir(longdir, 0700) != 0) {
            if (errno == ENAMETOOLONG) {
                long_path_available = 0;
                break;
            }
            fail("create long path component");
        }
    }
    if (long_path_available) {
        char longfile[2048];
        snprintf(longfile, sizeof(longfile), "%s/document.txt", longdir);
        fp = fopen(longfile, "w");
        if (!fp || fclose(fp) != 0) fail("create long backup target");
        if (!backupWrite(longfile, content, sizeof(content) - 1)) fail("long-path backupWrite");
        backupRemove(longfile);
        unlink(longfile);
    }

    unlink(filename);
    unlink(config);
    puts("settings/backup tests: ok");
    return 0;
}
