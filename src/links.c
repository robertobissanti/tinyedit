#define _DEFAULT_SOURCE
#include "links.h"
#include "alloc.h"
#include "utf8.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int32_t next(const char *text, int32_t at, int32_t length) {
    return at + (int32_t)utf8NextCharLen(text, (size_t)at, (size_t)length);
}

uint8_t linksIsWeb(const char *target) {
    size_t prefix = !strncasecmp(target, "https://", 8) ? 8 :
        !strncasecmp(target, "http://", 7) ? 7 : 0;
    if (!prefix || !target[prefix]) return 0;
    for (const unsigned char *p = (const unsigned char *)target; *p; p++)
        if (*p <= 32 || *p == 127) return 0;
    return 1;
}

uint8_t linksFind(const char *text, int32_t length, int32_t cursor,
    struct textLink *link) {
    if (cursor < 0 || cursor >= length) return 0;
    int32_t at = 0;
    while (at < length) {
        int32_t start = at, end = at, first = at, last = at;
        if (text[at] == '\\') {
            at = next(text, at, length);
            if (at < length) at = next(text, at, length);
            continue;
        }
        if (text[at] == '`') {
            int32_t count = 0;
            while (at < length && text[at] == '`') { at++; count++; }
            while (at < length) {
                if (text[at] != '`') { at = next(text, at, length); continue; }
                int32_t run = 0;
                while (at < length && text[at] == '`') { at++; run++; }
                if (run == count) break;
            }
            continue;
        }
        if (text[at] == '[') {
            int32_t depth = 1;
            at++;
            while (at < length && depth) {
                if (text[at] == '\\') {
                    at++;
                    if (at < length) at = next(text, at, length);
                    continue;
                }
                if (text[at] == '[') depth++;
                if (text[at] == ']') depth--;
                at = next(text, at, length);
            }
            if (depth) return 0;
            if (at >= length || text[at] != '(') continue;
            at++;
            while (at < length && text[at] == ' ') at++;
            uint8_t angled = at < length && text[at] == '<';
            if (angled) at++;
            first = at;
            depth = 0;
            while (at < length) {
                if (text[at] == '\\') {
                    at++;
                    if (at < length) at = next(text, at, length);
                    continue;
                }
                if (angled ? text[at] == '>' :
                    (text[at] == ')' && !depth) || isspace((unsigned char)text[at])) break;
                if (!angled && text[at] == '(') depth++;
                if (!angled && text[at] == ')') depth--;
                at = next(text, at, length);
            }
            last = at;
            if (angled) {
                if (at >= length || text[at] != '>') continue;
                at++;
            }
            while (at < length && text[at] == ' ') at++;
            if (at < length && (text[at] == '"' || text[at] == '\'')) {
                char quote = text[at++];
                while (at < length && text[at] != quote) {
                    if (text[at] == '\\') at++;
                    if (at < length) at = next(text, at, length);
                }
                if (at < length) at++;
                while (at < length && text[at] == ' ') at++;
            }
            if (at >= length || text[at] != ')' || last == first) continue;
            end = ++at;
        } else if ((length - at >= 7 && !strncasecmp(text + at, "http://", 7)) ||
                   (length - at >= 8 && !strncasecmp(text + at, "https://", 8))) {
            first = at;
            int32_t depth = 0;
            while (at < length && !isspace((unsigned char)text[at]) &&
                   text[at] != '<' && text[at] != '>' && text[at] != '"' && text[at] != '`') {
                if (text[at] == '(') depth++;
                if (text[at] == ')' && !depth) break;
                if (text[at] == ')') depth--;
                at = next(text, at, length);
            }
            last = at;
            while (last > first && strchr(".,;:!?", text[last - 1])) last--;
            end = last;
        } else { at = next(text, at, length); continue; }
        if (cursor >= start && cursor < end) {
            *link = (struct textLink){start, end, first, last};
            return 1;
        }
    }
    return 0;
}

char *linksTarget(const char *text, const struct textLink *link) {
    size_t length = (size_t)(link->target_end - link->target_start);
    char *target = teMalloc(teSizeAdd(length, 1));
    size_t out = 0;
    for (int32_t at = link->target_start; at < link->target_end;) {
        if (text[at] == '\\' && at + 1 < link->target_end &&
            ispunct((unsigned char)text[at + 1])) at++;
        size_t step = utf8NextCharLen(text, (size_t)at, (size_t)link->target_end);
        unsigned char c = (unsigned char)text[at];
        if (c < 32 || c == 127) { free(target); return NULL; }
        memcpy(target + out, text + at, step);
        out += step;
        at += (int32_t)step;
    }
    target[out] = '\0';
    return target;
}

static int32_t hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *linksDecode(const char *text) {
    size_t length = strlen(text), out = 0;
    char *decoded = teMalloc(teSizeAdd(length, 1));
    for (size_t at = 0; at < length;) {
        unsigned char c = (unsigned char)text[at++];
        if (c == '%' && length - at >= 2 && hex((unsigned char)text[at]) >= 0 &&
            hex((unsigned char)text[at + 1]) >= 0) {
            c = (unsigned char)(hex((unsigned char)text[at]) * 16 + hex((unsigned char)text[at + 1]));
            at += 2;
        }
        if (c < 32 || c == 127) { free(decoded); return NULL; }
        decoded[out++] = (char)c;
    }
    decoded[out] = '\0';
    return decoded;
}

char *linksHeadingSlug(const char *text, int32_t length) {
    char *slug = teMalloc(teSizeAdd((size_t)length, 1));
    int32_t at = 0;
    size_t out = 0;
    while (at < length && text[at] == ' ') at++;
    while (at < length && text[at] == '#') at++;
    while (at < length && text[at] == ' ') at++;
    while (length > at && (text[length - 1] == ' ' || text[length - 1] == '#')) length--;
    while (at < length) {
        size_t step = utf8NextCharLen(text, (size_t)at, (size_t)length);
        unsigned char c = (unsigned char)text[at];
        if (c >= 128) { memcpy(slug + out, text + at, step); out += step; }
        else if (isalnum(c) || c == '_' || c == '-') slug[out++] = (char)tolower(c);
        else if (c == ' ' || c == '\t') slug[out++] = '-';
        at += (int32_t)step;
    }
    slug[out] = '\0';
    return slug;
}

uint8_t linksOpenWeb(const char *target) {
    if (!linksIsWeb(target)) { errno = EINVAL; return 0; }
    int channel[2];
    if (pipe(channel) < 0) return 0;
    if (fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno; close(channel[0]); close(channel[1]); errno = error; return 0;
    }
    pid_t child = fork();
    if (child == 0) {
        close(channel[0]);
        pid_t launcher = fork();
        if (launcher == 0) {
            int sink = open("/dev/null", O_RDWR);
            if (sink >= 0) {
                (void)dup2(sink, STDIN_FILENO);
                (void)dup2(sink, STDOUT_FILENO);
                (void)dup2(sink, STDERR_FILENO);
                if (sink > STDERR_FILENO) close(sink);
            }
#ifdef __APPLE__
            execlp("open", "open", target, (char *)NULL);
#else
            execlp("xdg-open", "xdg-open", target, (char *)NULL);
#endif
            int error = errno;
            (void)write(channel[1], &error, sizeof(error));
            _exit(127);
        }
        if (launcher < 0) {
            int error = errno;
            (void)write(channel[1], &error, sizeof(error));
        }
        _exit(launcher < 0 ? 127 : 0);
    }
    close(channel[1]);
    if (child < 0) { int error = errno; close(channel[0]); errno = error; return 0; }
    int error = 0;
    ssize_t count;
    do { count = read(channel[0], &error, sizeof(error)); } while (count < 0 && errno == EINTR);
    close(channel[0]);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    if (count != 0) { if (error) errno = error; return 0; }
    return 1;
}
