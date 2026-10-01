/* 授权上下文的唯一合法来源：磁盘上的 manifest（brain: qzos-service-boundary）
 *
 * 曾经的形状是错的：op:app 带着 `id` + `perms` 数组从渲染桥进来，host 直接
 * 收下当授权用。而应用与 shell 共享同一个 QuickJS 全局，`ui` 是个全局对象，
 * 于是**任何应用都能自己调 ui.setApp('self', ['storage'])** 给自己授权。
 *
 * 实测（perms: [] 的应用）：
 *     DECLARED=[]  →  ui.setApp('escaper', ['storage'])  →  SETAPP ACCEPTED
 *     →  qzos-services: app 'escaper' authorized (1 caps)
 *     →  sys.storage.statfs *** ALLOWED ***
 *
 * 不需要外部进程、不需要 socket。授权判定搬到了服务面（对），但**判定的输入
 * 由攻击者提供**——挂错位置和输入不可信是两件事，前者上次修了，后者这次修。
 *
 * 所以：JS 可以**点名**哪个应用在跑，不能**声明**它能做什么。perms 一律由
 * 本文件从 <trusted-root>/<id>/app.json 重新推导。
 */
#ifndef QZOS_APPAUTH_H
#define QZOS_APPAUTH_H

#include <stdbool.h>

/* 受信应用根目录（通常是 <QZ_JS_DIR>/apps）。宿主启动时设一次。
 *
 * 设为空 = 没有任何应用能拿到能力（fail-closed）。这不是「忘了设」的惩罚，
 * 而是刻意的：授权的来源是磁盘，而磁盘上有没有可信目录这件事必须显式告知。 */
void qzos_apputil_set_apps_root(const char *root);
const char *qzos_apputil_apps_root(void);

/* app.json 解析上限。manifest 是小文件；给一个上限是为了「一个 4MB 的
 * app.json 把宿主内存吃光」这种失败模式不成立。 */
#define QZOS_APP_JSON_MAX 4096

/* 从 <apps_root>/<id>/app.json 推导能力，写进 out。返回 true 表示解析成功
 * （caps 可能仍然是 0 —— 零能力是合法结果，不是错误）。
 *
 * 任何一步不满足都返回 false 且 **out 置空**：读不到就当没有权限，
 * 绝不猜、绝不沿用上一次的授权。 */
bool qzos_apputil_caps_from_manifest(const char *id, char out[][16], int max, int *n);

/* id 是否可作为单级目录名使用。拒绝 '/'、'..'、前导 '.'、空串、过长——
 * 路径穿越必须在拼路径**之前**就挡住，拼接后 realpath 检查太晚。 */
bool qzos_apputil_id_is_safe(const char *id);

#endif /* QZOS_APPAUTH_H */
