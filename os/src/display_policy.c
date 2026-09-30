/*
 * display_policy.c — e-ink 刷新策略实现（C99）
 */
#include "display_policy.h"

#include <stdio.h>
#include <stdlib.h>

void qzos_policy_cfg_from_env(qzos_policy_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->full_every = 20;
    cfg->fast_threshold = 0;   /* 默认不用快速波形：残影风险 */
    cfg->auto_commit = true;

    const char *e;
    if ((e = getenv("QZ_FULL_EVERY")) != NULL && *e) {
        cfg->full_every = atoi(e);
    }
    if ((e = getenv("QZ_FAST_THRESHOLD")) != NULL && *e) {
        cfg->fast_threshold = atoi(e);
    }
    if ((e = getenv("QZ_AUTO_COMMIT")) != NULL && *e) {
        cfg->auto_commit = (*e == '1' || *e == 'y' || *e == 'Y');
    }
}

void qzos_policy_init(qzos_policy_t *p, const qzos_policy_cfg_t *cfg)
{
    if (!p) return;
    p->cfg = cfg ? *cfg : (qzos_policy_cfg_t){ 20, 0, true };
    p->commits_since_full = 0;
    p->commits_total = 0;
    p->fulls_total = 0;
    p->pending_full = true;   /* 开机首帧必须全刷：屏上残留旧系统内容 */
}

void qzos_policy_request_full(qzos_policy_t *p)
{
    if (p) p->pending_full = true;
}

qzos_action_t qzos_policy_decide(qzos_policy_t *p, const qzos_panel_t *panel,
                                 int32_t dirty_pixels, int32_t total_pixels,
                                 qzos_waveform_t *out_wf)
{
    if (out_wf) *out_wf = QZOS_WF_AUTO;
    if (!p || !panel) return QZOS_ACT_NONE;

    /* 显式请求优先：开机、清屏、模式切换 */
    if (p->pending_full) {
        if (!panel->cap_full_refresh) {
            /* 面板没有全刷机制：退化为局部更新，交给驱动选波形 */
            return QZOS_ACT_FAST;
        }
        if (out_wf) *out_wf = QZOS_WF_FULL;
        return QZOS_ACT_FULL;
    }

    /* 无变化：绝不动屏。e-ink 每次刷新都有寿命成本。 */
    if (dirty_pixels <= 0) return QZOS_ACT_NONE;

    /* 不自动提交：等显式 refresh op */
    if (!p->cfg.auto_commit) return QZOS_ACT_NONE;

    /* 周期全刷：残影只能靠全刷清除 */
    if (p->cfg.full_every > 0 &&
        p->commits_since_full + 1 >= p->cfg.full_every) {
        if (panel->cap_full_refresh) {
            if (out_wf) *out_wf = QZOS_WF_FULL;
            return QZOS_ACT_FULL;
        }
    }

    /* 快速波形只在「脏区确实小」时显式要求：此时省时收益明确。
     * 脏区大时留在 WF_AUTO，让驱动按自己的差分逻辑选——强制快速波形
     * 省不了多少时间，残影风险却一样。 */
    if (p->cfg.fast_threshold > 0 && panel->cap_waveform_select &&
        total_pixels > 0) {
        int32_t dirty_pct = (dirty_pixels * 100) / total_pixels;
        if (dirty_pct <= p->cfg.fast_threshold && out_wf) {
            *out_wf = QZOS_WF_FAST;
        }
    }

    return QZOS_ACT_FAST;
}

void qzos_policy_on_committed(qzos_policy_t *p, qzos_action_t act)
{
    if (!p) return;
    if (act == QZOS_ACT_NONE) return;

    p->commits_total++;
    if (act == QZOS_ACT_FULL) {
        p->commits_since_full = 0;
        p->fulls_total++;
        p->pending_full = false;
    } else {
        p->commits_since_full++;
        p->pending_full = false;
    }
}

qzos_waveform_t qzos_policy_waveform(const qzos_panel_t *panel, qzos_action_t act)
{
    if (!panel) return QZOS_WF_AUTO;
    switch (act) {
    case QZOS_ACT_FULL: return QZOS_WF_FULL;
    case QZOS_ACT_FAST: return panel->cap_waveform_select ? QZOS_WF_FAST : QZOS_WF_AUTO;
    case QZOS_ACT_NONE:
    default: return QZOS_WF_AUTO;
    }
}

const char *qzos_action_name(qzos_action_t a)
{
    switch (a) {
    case QZOS_ACT_FULL: return "full";
    case QZOS_ACT_FAST: return "fast";
    case QZOS_ACT_NONE:
    default: return "none";
    }
}
