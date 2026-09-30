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
    /* e-ink 黑白样式：默认主题按钮是灰底（L8=124 < 128 全阈成黑、文字不
     * 反白）。统一为白底黑字黑框、去圆角/阴影——1bpp 下才有清晰对比。 */
    if (strcmp(type, "btn") == 0) {
        lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
        lv_obj_set_style_bg_color(obj, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(obj, lv_color_white(), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(obj, lv_color_black(), 0);
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_radius(obj, 0, 0);
        lv_obj_set_style_shadow_width(obj, 0, 0);
        lv_obj_set_style_shadow_width(obj, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(obj, 2, 0);
    } else {
        lv_obj_set_style_text_color(obj, lv_color_black(), 0);
        lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_radius(obj, 0, 0);
        lv_obj_set_style_shadow_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
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

static void op_rpc(cJSON *j)
{
    cJSON *rid = cJSON_GetObjectItem(j, "rid");
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(j, "method"));
    if (!method) return;
    int id = cJSON_IsNumber(rid) ? rid->valueint : 0;
    cJSON *params = cJSON_GetObjectItem(j, "params");
    char *pstr = params ? cJSON_PrintUnformatted(params) : NULL;

    int *ridp = malloc(sizeof(int));
    if (ridp) {
        *ridp = id;
        qzos_services_rpc(method, pstr ? pstr : "", rpc_done, ridp);
    }
    if (pstr) free(pstr);
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
        /* 非 op 消息（如 {evt:'error'}）：打到 stderr 便于诊断 */
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
        if (s && qzos_input_group()) lv_group_focus_obj(s->obj);
    } else if (strcmp(op, "refresh") == 0) {
        cJSON *full = cJSON_GetObjectItem(j, "full");
        /* 显式重绘 + 可选全刷波形。
         * 注意 lv_timer_handler 不会自行发起重绘，必须显式失效化。 */
        qzos_display_repaint();
        if (cJSON_IsTrue(full)) qzos_display_full_refresh();
    } else if (strcmp(op, "rpc") == 0) {
        op_rpc(j);
    } else {
        qzos_bridge_sendf("{\"evt\":\"error\",\"msg\":\"unknown op: %s\"}", op);
    }
    cJSON_Delete(j);
}
