/* appauth.c — 从磁盘 manifest 推导应用能力（brain: qzos-service-boundary）
 *
 * 这里的每一条规则都与 os/js/apkg.js 一一对应，但**方向相反**：
 * apkg.js 那份用于**发现与展示**（决定要不要把应用列出来、UI 上显不显示按钮），
 * 本文件这份用于**强制**。JS 那份被改坏时最多让界面显示出错的按钮；本文件
 * 被改坏才是安全问题。所以两份各写一份是有意的冗余，靠闸门核对，不靠约定。
 *
 * 分工的界线：**JS 可以点名一个应用，不能决定它能做什么。**
 */
#include "appauth.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* 与 services.c 的 s_known_caps 保持一致。两处各一份的理由同上：
 * 「同步两张表」靠闸门，不靠约定。 */
static const char *const s_known_caps[] = {
    "info", "storage", "settings", "net", "power"
};
#define N_KNOWN_CAPS ((int)(sizeof(s_known_caps) / sizeof(s_known_caps[0])))

static char s_apps_root[512];

void qzos_apputil_set_apps_root(const char *root)
{
    if (!root || !*root) { s_apps_root[0] = '\0'; return; }
    snprintf(s_apps_root, sizeof(s_apps_root), "%s", root);
}

const char *qzos_apputil_apps_root(void)
{
    return s_apps_root[0] ? s_apps_root : NULL;
}

bool qzos_apputil_id_is_safe(const char *id)
{
    if (!id || !*id) return false;
    size_t n = strlen(id);
    if (n == 0 || n >= 64) return false;
    /* 前导 '.' 一律拒：'.' 与 '..' 是目录穿越的两把钥匙，而隐藏目录在
     * 这个系统里没有正当用途，一并挡掉省得分别论证。 */
    if (id[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;   /* '/' 与 '\\' 落在这一条 */
    }
    return true;
}

bool qzos_apputil_caps_from_manifest(const char *id, char out[][16], int max, int *n)
{
    if (n) *n = 0;
    if (out && max > 0) out[0][0] = '\0';

    /* 缺省全拒。没设受信根 = 磁盘上没有被告知哪里可信，于是没有应用
     * 能拿到能力。这和「用户目录 fail-closed」是同一条原则。 */
    if (!s_apps_root[0] || !qzos_apputil_id_is_safe(id)) {
        fprintf(stderr, "qzos-apputil: refuse id '%s' (root=%s)\n",
                id ? id : "(null)", s_apps_root[0] ? s_apps_root : "(unset)");
        return false;
    }

    char path[640];
    snprintf(path, sizeof(path), "%s/%s/app.json", s_apps_root, id);

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* 读不到就是读不到。不猜、不沿用旧值 —— 上一个应用的授权漏给下一个，
         * 那正是「静默给错」的另一种形状。 */
        fprintf(stderr, "qzos-apputil: no manifest at %s\n", path);
        return false;
    }
    char buf[QZOS_APP_JSON_MAX];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    int overlong = !feof(f);
    fclose(f);
    buf[got] = '\0';
    if (overlong) {
        fprintf(stderr, "qzos-apputil: %s exceeds %d bytes — refuse\n",
                path, QZOS_APP_JSON_MAX);
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        fprintf(stderr, "qzos-apputil: %s is not valid JSON — refuse\n", path);
        return false;
    }

    /* manifest 里的 id 必须与目录名一致。少了这一条，一个目录可以挂着
     * **别的**应用的授权：把 victim 的 manifest 复制进自己的目录就行。
     * 目录名是路径给的、不可伪造，manifest 里的 id 是文件给的、可改。 */
    cJSON *jid = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsString(jid) || strcmp(jid->valuestring, id) != 0) {
        fprintf(stderr, "qzos-apputil: %s id mismatch — refuse\n", path);
        cJSON_Delete(root);
        return false;
    }

    cJSON *perms = cJSON_GetObjectItem(root, "perms");
    if (!cJSON_IsArray(perms)) { cJSON_Delete(root); return true; } /* 零能力合法 */

    int got_caps = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, perms) {
        if (!cJSON_IsString(it)) continue;
        const char *c = it->valuestring;
        size_t cl = strlen(c);
        if (cl == 0 || cl >= 16) continue;
        bool known = false;
        for (int k = 0; k < N_KNOWN_CAPS; k++) {
            if (strcmp(c, s_known_caps[k]) == 0) { known = true; break; }
        }
        /* 表外能力：不静默忽略、也不部分放行。整条 manifest 作废 ——
         * 静默忽略会让应用带着残缺授权在系统里跑而作者不知情。 */
        if (!known) {
            fprintf(stderr, "qzos-apputil: %s declares unknown cap '%s' — refuse\n",
                    path, c);
            cJSON_Delete(root);
            if (n) *n = 0;
            if (out && max > 0) out[0][0] = '\0';
            return false;
        }
        if (got_caps < max) {
            snprintf(out[got_caps], 16, "%s", c);
            got_caps++;
        }
    }
    cJSON_Delete(root);
    if (n) *n = got_caps;
    fprintf(stderr, "qzos-apputil: '%s' -> %d cap(s) from manifest\n", id, got_caps);
    return true;
}
