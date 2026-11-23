// Background GitHub release check for mtf-recomp.exe (non-blocking, silent on network errors).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "update_check.h"

#define RELEASES_URL "https://github.com/rafael-fj/mtf-recompiled/releases/latest"

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static UpdateInfo g_info;
static int g_started;

static void parse_semver(const char *s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!s) return;
    while (*s && !isdigit((unsigned char)*s)) s++;
    for (int i = 0; i < 3 && *s; i++) {
        out[i] = (int)strtol(s, (char **)&s, 10);
        if (*s == '.') s++;
        else break;
    }
}

static int is_newer(const char *candidate, const char *current) {
    int a[3], b[3];
    parse_semver(candidate, a);
    parse_semver(current, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 0;
}

static int json_str_field(const char *json, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

static void set_available(const char *tag, const char *url) {
    plat_mutex_lock(&g_lock);
    g_info.available = 1;
    snprintf(g_info.tag, sizeof g_info.tag, "%s", tag);
    snprintf(g_info.url, sizeof g_info.url, "%s", url && *url ? url : RELEASES_URL);
    plat_mutex_unlock(&g_lock);
    rt_log("update_check: new release %s available (current %s)", tag, rt_port_version);
}

static void update_worker(void *arg) {
    (void)arg;
    HINTERNET ses = WinHttpOpen(L"MTF-Recomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return;
    WinHttpSetTimeouts(ses, 4000, 4000, 4000, 5000);
    HINTERNET con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        return;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", L"/repos/rafael-fj/mtf-recompiled/releases/latest", NULL,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return;
    }
    const wchar_t *hdr = L"Accept: application/vnd.github+json\r\n";
    BOOL ok = WinHttpSendRequest(req, hdr, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, NULL);
    DWORD status = 0, slen = sizeof status;
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &slen, NULL);
    if (ok && status == 200) {
        char buf[16384];
        size_t total = 0;
        DWORD avail = 0;
        while (total + 1 < sizeof buf && WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            DWORD to_read = avail;
            if (total + to_read >= sizeof buf) to_read = (DWORD)(sizeof buf - 1 - total);
            DWORD got = 0;
            if (!WinHttpReadData(req, buf + total, to_read, &got) || !got) break;
            total += got;
        }
        buf[total] = 0;
        char tag[32] = {0}, url[256] = {0};
        if (json_str_field(buf, "tag_name", tag, sizeof tag) && is_newer(tag, rt_port_version)) {
            json_str_field(buf, "html_url", url, sizeof url);
            set_available(tag, url);
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
}

void update_check_start(void) {
    plat_mutex_lock(&g_lock);
    if (g_started) {
        plat_mutex_unlock(&g_lock);
        return;
    }
    g_started = 1;
    plat_mutex_unlock(&g_lock);

    const char *e = cfg_get("MTF_UPDATE_CHECK");
    if (e && !strcmp(e, "0")) return;

    const char *test_tag = cfg_get("MTF_UPDATE_TEST_TAG");
    if (test_tag && *test_tag) {
        if (is_newer(test_tag, rt_port_version)) set_available(test_tag, RELEASES_URL);
        return;
    }

    plat_thread_spawn(update_worker, NULL, "update-check", 0);
}

int update_check_status(UpdateInfo *out) {
    plat_mutex_lock(&g_lock);
    if (out) *out = g_info;
    int avail = g_info.available;
    plat_mutex_unlock(&g_lock);
    return avail;
}

void update_check_open_url(void) {
    UpdateInfo info;
    if (!update_check_status(&info)) return;
    ShellExecuteA(NULL, "open", info.url, NULL, NULL, SW_SHOWNORMAL);
}
