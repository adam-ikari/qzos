/*
 * bridge.c — JSON UI protocol: JS shell <-> LVGL objects
 *
 * JS -> host (postMessage JSON):
 *   {"op":"create","id":"x","type":"label|btn|box|list|ta","parent":"root"|id}
 *   {"op":"set","id":"x","text":"...","x":0,"y":0,"w":10,"h":10,
 *    "font":"sm|md|lg","align":"center|left|...","hidden":false}
 *   {"op":"add","id":"x","child":"y"}          reparent
 *   {"op":"on","id":"x","event":"click|value"} subscribe
 *   {"op":"del","id":"x"}                      delete subtree
 *   {"op":"focus","id":"x"}                    group focus
 *   {"op":"clear"}                             wipe active screen children
 *   {"op":"refresh","full":true}               request e-ink refresh
 *   {"op":"rpc","rid":1,"method":"sys.info","params":{...}}
 *   {"op":"app","id":"notepad","perms":["storage"]}   declare active app
 *   {"op":"app","id":null,"perms":[]}                 clear (back to desktop)
 *
 * host -> JS (qz_post_message):
 *   {"evt":"click","id":"x"} / {"evt":"value","id":"x","text":"..."}
 *   {"evt":"key","key":"home|back|..."}       system keys
 *   {"evt":"rpc","rid":1,"ok":true,"result":...}
 *   {"evt":"error","msg":"..."}
 *
 * Threading: everything runs on the host loop thread (message_cb fires while
 * pumping the injected uv loop), so plain registry, no locks.
 */
#include "qzos.h"

#include <cJSON.h>
#include <lvgl.h>
#include <qzjs/qzjs.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OBJS 256

typedef struct {
    char id[48];
    lv_obj_t *obj;
    unsigned sub_events; /* bitmask: SUB_CLICK | SUB_VALUE */
} slot_t;

static slot_t s_slots[MAX_OBJS];
static qz_t *s_rt;

enum { SUB_CLICK = 1, SUB_VALUE = 2 };

/* 授权上下文**不存这里**。
 *
 * 早期它住在 bridge.c 的一个静态结构里，由 op_rpc 逐次比对。那是历史偶然：
 * 当时只有 sys.info 一个方法，op_rpc 顺手就检查了；后来加了能力才暴露出那个
 * 位置不对——渲染桥是纯命令通道，在架构上不该承载能力。现在上下文归
 * services.c（唯一的能力边界），op:app 只负责把 perms 转交过去。
 */
#define MAX_CAPS 8

void qzos_bridge_set_rt(qz_t *rt)
{
    s_rt = rt;
}

void qzos_bridge_sendf(const char *fmt, ...)
{
    if (!s_rt) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    qz_post_message(s_rt, buf, (size_t)n);
}

void qzos_bridge_send_key(const char *key)
{
    qzos_bridge_sendf("{\"evt\":\"key\",\"key\":\"%s\"}", key);
}

/* ---- registry ---- */

static slot_t *find_slot(const char *id)
{
    for (int i = 0; i < MAX_OBJS; i++)
        if (s_slots[i].obj && strcmp(s_slots[i].id, id) == 0) return &s_slots[i];
    return NULL;
}

static slot_t *alloc_slot(const char *id, lv_obj_t *obj)
{
    slot_t *s = find_slot(id);
    if (s) { s->obj = obj; return s; }
    for (int i = 0; i < MAX_OBJS; i++) {
        if (!s_slots[i].obj) {
            snprintf(s_slots[i].id, sizeof(s_slots[i].id), "%s", id);
            s_slots[i].obj = obj;
            s_slots[i].sub_events = 0;
            return &s_slots[i];
        }
    }
    return NULL;
}

static lv_obj_t *resolve_parent(const char *name)
{
    if (!name || strcmp(name, "root") == 0 || strcmp(name, "screen") == 0)
        return lv_screen_active();
    slot_t *s = find_slot(name);
    return s ? s->obj : lv_screen_active();
}

static const lv_font_t *font_by_name(const char *n)
{
    if (!n) return NULL;
    if (strcmp(n, "sm") == 0) return &fusion_pixel_10;
    if (strcmp(n, "xs") == 0) return &fusion_pixel_8;
    if (strcmp(n, "md") == 0) return &fusion_pixel_12;
    if (strcmp(n, "lg") == 0) return &fusion_pixel_12;
    return NULL;
}

/* ---- LVGL events -> JSON ---- */

typedef struct {
    slot_t *slot;
    int kind; /* SUB_CLICK | SUB_VALUE */
} evt_ctx_t;

static void lv_event_to_json(lv_event_t *e)
{
    evt_ctx_t *ctx = (evt_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->slot || !ctx->slot->obj) return;
    if (ctx->kind == SUB_CLICK) {
        qzos_bridge_sendf("{\"evt\":\"click\",\"id\":\"%s\"}", ctx->slot->id);
    } else if (ctx->kind == SUB_VALUE) {
        const char *txt = "";
        lv_obj_t *o = ctx->slot->obj;
        if (lv_obj_check_type(o, &lv_label_class)) txt = lv_label_get_text(o);
        else if (lv_obj_check_type(o, &lv_textarea_class)) txt = lv_textarea_get_text(o);
        qzos_bridge_sendf("{\"evt\":\"value\",\"id\":\"%s\",\"text\":\"%s\"}",
                          ctx->slot->id, txt);
    }
}

static bool add_sub(slot_t *s, int kind)
{
    if (s->sub_events & kind) return true;
    evt_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return false;
    ctx->slot = s;
    ctx->kind = kind;
    lv_obj_add_event_cb(s->obj, lv_event_to_json,
                        kind == SUB_CLICK ? LV_EVENT_CLICKED : LV_EVENT_VALUE_CHANGED, ctx);
    s->sub_events |= kind;
    return true;
}

/* ---- ops ---- */

static void op_create(cJSON *j)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(j, "parent"));
    if (getenv("QZ_UI_DEBUG")) {
        char *raw = cJSON_PrintUnformatted(j);
        fprintf(stderr, "[ui] create %s\n", raw ? raw : "?");
        free(raw);
    }
    if (!id || !type) {
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"create: id/type required\"}");
        return;
    }
    lv_obj_t *p = resolve_parent(parent);
    lv_obj_t *obj = NULL;

    if (strcmp(type, "label") == 0) obj = lv_label_create(p);
    else if (strcmp(type, "btn") == 0) obj = lv_button_create(p);
    else if (strcmp(type, "box") == 0) obj = lv_obj_create(p);
    else if (strcmp(type, "list") == 0) obj = lv_list_create(p);
    else if (strcmp(type, "ta") == 0) obj = lv_textarea_create(p);
    else if (strcmp(type, "cb") == 0) obj = lv_checkbox_create(p);
    else {
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"unknown type: %s\"}", type);
        return;
    }

    slot_t *s = alloc_slot(id, obj);
    if (!s) {
        lv_obj_delete(obj);
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"object table full\"}");
        return;
    }
    /* ---- e-ink 样式：不继承默认主题，自己定死视觉 ----
     *
     * 理由不是审美，是三条实测得来的硬事实：
     *
     *  1) 1bpp 下主题的视觉语言基本是浪费甚至有害：灰底（L8=124 < 128，会被
     *     整个阈成黑，文字反而不显）、圆角、阴影、外发光——这些是为了在彩色
     *     背光屏上营造层次，在只有两色的墨水屏上要么被阈值吃掉，要么白占刷屏
     *     面积。
     *  2) **主题的 style transition 让每次点击多刷 3~4 次屏**。按钮挂着
     *     transition_delayed / transition_normal（TRANSITION_TIME=120ms），
     *     而 LV_STATE_PRESSED 带 recolor 与 shadow，按下/抬起各触发一段逐帧
     *     动画 -> 逐帧失效化 -> 逐帧提交。实测按一个**留在屏上**的按钮会多出
     *     3~4 次提交（相邻两次正好差一个 LV_DEF_REFR_PERIOD，在两种渲染之间
     *     来回跳），而 1bpp 屏上"按下"与"常态"本来就看不出区别。
     *     逐个属性去中和（recolor_opa / shadow_width / outline_* /
     *     transform_*）是打地鼠：主题样式按状态挂在不同 selector 上，漏一个
     *     就又刷一次，换个 LVGL 版本还会再冒出来。
     *     诚实说明：**具体是哪一个属性造成的，没有单独隔离出来**——变异测试
     *     显示"保留 remove_style_all 但把这组属性显式补齐"同样不再抖动，所以
     *     起作用的是扁平化属性本身。remove_style_all 仍然保留，为的是让
     *     "不继承为背光屏设计的视觉语言"成为结构性约束，而不是一份要逐条维护
     *     的属性清单；真正把回归锁住的是 verify-input.sh 的刷新预算断言。
     *  3) 同理，textarea 光标的 400ms 闪烁（主题的 ta_cursor 样式挂在
     *     LV_PART_CURSOR|LV_STATE_FOCUSED 上）在 remove_style_all 之后自然
     *     消失；下面仍然显式把 anim_duration 设 0，作为"光标不许闪"的显式
     *     声明，免得以后有人又把主题样式加回来。
     *
     * 字体不受影响：字体是**继承**属性（从 screen 上的主题继承），移除控件
     * 自身的样式不会让它退回默认字形。
     */
    lv_obj_remove_style_all(obj);

    /* 所有控件共同的基线：白底黑字、无框无角无阴影无 padding。 */
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(obj, lv_color_black(), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);

    if (strcmp(type, "btn") == 0) {
        /* 按钮是唯一必须"看得见焦点"的控件（键盘导航要告诉用户选中了哪个），
         * 所以给 1px 黑框。焦点指示用加粗边框而不是颜色变化——1bpp 没有颜色，
         * 只有黑白粗细之分；也不用主题的 outline_primary（彩色 + 带动画）。 */
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_pad_all(obj, 2, 0);
        /* 焦点指示：边框加粗。**两个 state 都要给**——LVGL 只在 group 知道
         * 自己的 indev 时才追加 LV_STATE_FOCUS_KEY（lv_obj.c 的
         * LV_EVENT_FOCUSED 分支要看 indev_type），拿不到就只给
         * LV_STATE_FOCUSED。只写 FOCUS_KEY 的后果是"焦点框看不见"，而在一块
         * 没有背光、全靠按键导航的墨水屏上，这等于用户不知道选中了哪个。 */
        lv_obj_set_style_border_color(obj, lv_color_black(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(obj, lv_color_black(), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUS_KEY);
    }
    if (strcmp(type, "ta") == 0) {
        /* 光标常亮不闪：仍然看得见插入点，但只在它真的移动时才刷一次屏。
         * 主题移除后 LV_PART_CURSOR 没有任何样式，光标会不可见，所以这里
         * 显式给它上色。 */
        lv_obj_set_style_anim_duration(obj, 0, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(obj, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_CURSOR);
        lv_obj_set_style_border_width(obj, 1, LV_PART_CURSOR);
        lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_CURSOR);
    }
    if (qzos_input_group() &&
        (strcmp(type, "btn") == 0 || strcmp(type, "ta") == 0 ||
         strcmp(type, "cb") == 0 || strcmp(type, "list") == 0)) {
        lv_group_add_obj(qzos_input_group(), obj);
    }
}

static void op_set(cJSON *j)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
    if (getenv("QZ_UI_DEBUG")) {
        char *raw = cJSON_PrintUnformatted(j);
        fprintf(stderr, "[ui] set %s\n", raw ? raw : "?");
        free(raw);
    }
    if (!id) return;
    slot_t *s = find_slot(id);
    if (!s) {
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"set: unknown id %s\"}", id);
        return;
    }
    lv_obj_t *o = s->obj;

    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    if (text) {
        if (lv_obj_check_type(o, &lv_label_class)) lv_label_set_text(o, text);
        else if (lv_obj_check_type(o, &lv_textarea_class)) lv_textarea_set_text(o, text);
        else if (lv_obj_check_type(o, &lv_button_class)) {
            lv_obj_t *cl = lv_obj_get_child(o, 0);
            if (!cl || !lv_obj_check_type(cl, &lv_label_class)) cl = lv_label_create(o);
            lv_label_set_text(cl, text);
        } else if (lv_obj_check_type(o, &lv_checkbox_class)) lv_checkbox_set_text(o, text);
    }

    cJSON *x = cJSON_GetObjectItem(j, "x");
    cJSON *y = cJSON_GetObjectItem(j, "y");
    if (cJSON_IsNumber(x) && cJSON_IsNumber(y))
        lv_obj_set_pos(o, x->valueint, y->valueint);
    cJSON *w = cJSON_GetObjectItem(j, "w");
    cJSON *h = cJSON_GetObjectItem(j, "h");
    /* 独立设置：不能取"当前高"兜底——label 未布局时 get_height()==0
     * （曾致 set_size(w,0) 标题不可见）。set_width 保留自动高度。 */
    if (cJSON_IsNumber(w)) lv_obj_set_width(o, w->valueint);
    if (cJSON_IsNumber(h)) lv_obj_set_height(o, h->valueint);

    const char *font = cJSON_GetStringValue(cJSON_GetObjectItem(j, "font"));
    const lv_font_t *f = font_by_name(font);
    if (f) lv_obj_set_style_text_font(o, f, 0);

    cJSON *hidden = cJSON_GetObjectItem(j, "hidden");
    if (cJSON_IsBool(hidden))
        lv_obj_set_hidden(o, cJSON_IsTrue(hidden));

    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "center"))) lv_obj_center(o);
}

static void op_del(cJSON *j)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
    if (!id) return;
    slot_t *s = find_slot(id);
    if (!s) return;
    lv_obj_t *obj = s->obj;
    s->obj = NULL;
    s->id[0] = '\0';
    lv_obj_delete(obj);
}

static void op_on(cJSON *j)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
    const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(j, "event"));
    if (!id || !ev) return;
    slot_t *s = find_slot(id);
    if (!s) {
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"on: unknown id %s\"}", id);
        return;
    }
    if (strcmp(ev, "click") == 0) add_sub(s, SUB_CLICK);
    else if (strcmp(ev, "value") == 0) add_sub(s, SUB_VALUE);
}

/* JS-facing async RPC completion: reply {evt:"rpc",rid,ok,result} to JS. */
static void rpc_done(int ok, const char *result, size_t len, void *u)
{
    int *rid = (int *)u;
    char *res = malloc(len + 1);
    if (res) {
        memcpy(res, result, len);
        res[len] = '\0';
        qzos_bridge_sendf("{\"evt\":\"rpc\",\"rid\":%d,\"ok\":%s,\"result\":%s}",
                          *rid, ok ? "true" : "false", res);
        free(res);
    }
    free(rid);
}

/* op:app — 声明/清空当前应用授权。**只是转交**给服务面：本文件不判定、
 * 也不保存状态——**perms 字段一律不看**，能力由 appauth.c 从磁盘 manifest
 * 推导。见表下面那段注释。 */
static void op_app(cJSON *j)
{
    cJSON *jid = cJSON_GetObjectItem(j, "id");
    const char *id = cJSON_IsString(jid) ? jid->valuestring : NULL;

    /* **只转发 id。perms 字段一律不看** —— 即使它带着、即使格式正确。
     *
     * 这里曾经把消息里的 perms 数组当授权收下，那是完整的提权漏洞：应用与
     * shell 共享同一个 QuickJS 全局，`ui` 是全局对象，于是任何应用都能调
     * `ui.setApp('self', ['storage'])` 给自己授权。实测一个 perms: [] 的
     * 应用真的调通了 sys.storage.statfs。不需要外部进程、不需要 socket。
     *
     * 授权的合法来源只有磁盘上的 <apps-root>/<id>/app.json（appauth.c）。
     * JS 可以点名一个应用，不能决定它能做什么。
     *
     * 带着 perms 字段来还**要记账**：静默忽略会让「我明明写了 perms」这件事
     * 完全不可见，于是下一个花半天找「为什么我的能力没生效」。 */
    cJSON *perms = cJSON_GetObjectItem(j, "perms");
    int claimed = (cJSON_IsArray(perms)) ? cJSON_GetArraySize(perms) : -1;

    qzos_services_note_app(id);

    char got[8][16];
    int n = qzos_services_app_caps(got, 8);
    if (id && claimed >= 0 && claimed != n) {
        fprintf(stderr,
                "qzos-bridge: op:app id='%s' claimed %d cap(s), host derived %d "
                "from manifest — claim ignored\n",
                id, claimed, n);
    }
}

static void op_rpc(cJSON *j)
{
    cJSON *rid = cJSON_GetObjectItem(j, "rid");
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(j, "method"));
    if (!method) return;
    int id = cJSON_IsNumber(rid) ? rid->valueint : 0;
    cJSON *params = cJSON_GetObjectItem(j, "params");
    char *pstr = params ? cJSON_PrintUnformatted(params) : NULL;

    /* 转发到系统服务面。**这里不做授权检查**。
     *
     * 授权检查已移到 services.c（brain: qzos-service-boundary）：JS 碰 C 只有
     * 服务面一条路，所以检查必须在那条路上。留在渲染桥的问题是——它只挡住了
     * 这一条通道，而「渲染桥」本身是纯命令通道、在架构上不该承载能力。
     *
     * 这一层现在只做一件事：把 JSON 变成服务面的方法调用。 */
    int *ridp = malloc(sizeof(int));
    if (ridp) {
        *ridp = id;
        qzos_services_rpc(method, pstr ? pstr : "", rpc_done, ridp);
    }
    if (pstr) free(pstr);
}

/* 焦点目标是否可编辑 -> group 是否进入编辑态。
 *
 * 这一步是把 LVGL 的"编辑态"暴露给 JS 的声明式入口：应用 focus 一个
 * textarea 就意味着"用户要在这里打字"，此后方向键归光标、字符键归文本。
 * 反过来 focus 按钮则退出编辑态，方向键重新用于移动焦点。
 * 没有它，方向键在非编辑态会被 input.c 翻译成 NEXT/PREV（见 input.c 里的
 * 说明），打字时光标就没法动了。 */
static bool obj_is_editable(lv_obj_t *o)
{
    return lv_obj_check_type(o, &lv_textarea_class);
}

/* ---- entry ---- */

void qzos_bridge_handle(const char *json, size_t len)
{
    cJSON *j = cJSON_ParseWithLength(json, len);
    if (!j) {
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"bad json\"}");
        return;
    }
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(j, "op"));
    if (!op) {
        /* qzjs 的引擎级错误帧：{"type":"error","error":"...-exited-unexpectedly"}。
         *
         * 这一帧曾经只被 fprintf 到 stderr 就丢掉——而它恰恰是「JS 引擎整个
         * 死了」的唯一信号。丢掉的后果实测得到：宿主继续健康地跑、面板上仍是
         * 完好无损的桌面画面、按键仍被读取，用户却按什么都没反应，只能重启
         * 设备。一个不报信的桌面比崩溃更糟，因为它骗人。
         *
         * 所以这里转成 rtError 投给 JS，并请宿主安排重建（见 main.c）。 */
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
        /* shell 的 {evt:'shellReady'}：它画完桌面了。放开开机提交按住，让首帧
         * 携带真正的桌面而不是空白屏（见 qzos_display_hold 的说明）。
         * 注意这跟引擎死亡是**相反**的方向：shellReady 之前屏幕故意是空白的，
         * 死亡时屏幕已经有过内容、我们要显式盖住。
         *
         * 只认 shellReady，不认宿主 boot 脚本发的那句 {evt:'ready'}：那里是
         * (0, eval)(shell.js) 之后立刻发的，而 shell.js 是 async IIFE，
         * 它的 await 还没跑完 —— 那个信号到达时桌面还没画。 */
        const char *evt = cJSON_GetStringValue(cJSON_GetObjectItem(j, "evt"));
        if (evt && strcmp(evt, "shellReady") == 0) {
            qzos_display_hold(0);
        } else if (evt && strcmp(evt, "bootFailed") == 0) {
            /* shell 根本没起来（读不到 / 语法错）。此时屏幕上什么都没有，
             * 而用户面对一台墨水屏一体机时看到纯白 = 「死机了，但我不知道为什么」。
             * 所以显式盖一块故障屏——和引擎死亡同一块屏、同一套信息。
             * 先放开提交按住，否则这块屏自己也落不下去。 */
            const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(j, "msg"));
            fprintf(stderr, "qzos-host: shell boot failed: %s\n", msg ? msg : "?");
            qzos_display_hold(0);
            qzos_show_boot_failed(msg);
        } else if (type && strcmp(type, "error") == 0) {
            const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(j, "error"));
            fprintf(stderr, "[js] engine error: %s\n", err ? err : "(none)");
            qzos_host_on_rt_death(err);
            cJSON_Delete(j);
            return;
        }
        /* 其余非 op 消息：打到 stderr 便于诊断 */
        char *raw = cJSON_PrintUnformatted(j);
        if (raw) {
            fprintf(stderr, "[js] %s\n", raw);
            free(raw);
        }
        cJSON_Delete(j);
        return;
    }

    if (strcmp(op, "create") == 0) op_create(j);
    else if (strcmp(op, "set") == 0) op_set(j);
    else if (strcmp(op, "del") == 0) op_del(j);
    else if (strcmp(op, "on") == 0) op_on(j);
    else if (strcmp(op, "clear") == 0) {
        lv_obj_clean(lv_screen_active());
        memset(s_slots, 0, sizeof(s_slots));
    } else if (strcmp(op, "focus") == 0) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
        slot_t *s = id ? find_slot(id) : NULL;
        if (s && qzos_input_group()) {
            /* 编辑态跟着焦点走：焦点落在可编辑控件上就进编辑态（可以打字、
             * 方向键归光标），否则退出（方向键归焦点移动）。 */
            lv_group_set_editing(qzos_input_group(), obj_is_editable(s->obj));
            lv_group_focus_obj(s->obj);
        }
    } else if (strcmp(op, "refresh") == 0) {
        cJSON *full = cJSON_GetObjectItem(j, "full");
        /* 显式重绘 + 可选全刷波形。
         * 注意 lv_timer_handler 不会自行发起重绘，必须显式失效化。 */
        qzos_display_repaint();
        if (cJSON_IsTrue(full)) qzos_display_full_refresh();
    } else if (strcmp(op, "rpc") == 0) {
        op_rpc(j);
    } else if (strcmp(op, "app") == 0) {
        op_app(j);
    } else {
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"unknown op: %s\"}", op);
    }
    cJSON_Delete(j);
}
