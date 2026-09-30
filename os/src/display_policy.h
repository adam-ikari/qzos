/*
 * display_policy.h — e-ink 刷新策略（纯决策，无 I/O、C99）
 *
 * 这一层回答唯一的问题：**这一帧要不要刷？用什么波形刷？**
 * 它不碰设备、不碰 LVGL、不碰帧缓冲内容，因此可以脱离硬件单测——
 * 而 e-ink 的刷新节奏恰恰是最需要回归保护的部分（刷多了伤屏寿命和电量，
 * 刷少了积残影）。
 *
 * e-ink 物理约束（决定了下面的策略形状）：
 *   - 每次更新都要黑白翻转若干次，功耗高、寿命有限 -> 能不刷就不刷
 *   - 残影只能靠全刷（多次黑白翻转）清除 -> 必须周期性全刷
 *   - 局部波形快但残影重，全刷慢但干净 -> 按「距上次全刷的提交数」权衡
 */
#ifndef QZOS_DISPLAY_POLICY_H
#define QZOS_DISPLAY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "panel.h"
#include "raster_1bpp.h"

/* ---- 策略配置 ---- */
typedef struct {
    int32_t full_every;     /* 每 N 次提交做一次全刷；<=0 = 只在显式请求时全刷 */
    int32_t fast_threshold; /* 脏像素占比低于此值走快速波形；0 = 禁用快速波形 */
    bool auto_commit;       /* true = 脏了就刷；false = 只在显式请求时刷 */
} qzos_policy_cfg_t;

/* ---- 决策结果 ---- */
typedef enum {
    QZOS_ACT_NONE = 0,   /* 不动屏（无变化 / 未到提交点） */
    QZOS_ACT_FAST,       /* 局部波形：快，残影轻 */
    QZOS_ACT_FULL        /* 全刷波形：清残影 */
} qzos_action_t;

/* ---- 策略状态 ---- */
typedef struct {
    qzos_policy_cfg_t cfg;
    int32_t commits_since_full;
    int32_t commits_total;
    int32_t fulls_total;
    bool pending_full;    /* 显式请求的全刷（开机、清屏、模式切换） */
} qzos_policy_t;

/* 读环境变量填配置：QZ_FULL_EVERY / QZ_FAST_THRESHOLD / QZ_AUTO_COMMIT */
void qzos_policy_cfg_from_env(qzos_policy_cfg_t *cfg);
void qzos_policy_init(qzos_policy_t *p, const qzos_policy_cfg_t *cfg);

/* 请求一次全刷（开机首帧、清屏、切换波形）。幂等。 */
void qzos_policy_request_full(qzos_policy_t *p);

/* 核心决策：给定「本帧脏区」与面板能力，决定动作与波形。
 * dirty_pixels == 0 表示帧与上次相同。
 * out_wf 可为 NULL（只要动作不要波形）。
 *
 * 波形由决策一并给出而非事后从动作推导，因为「局部更新」有两种含义：
 *   - 脏区很小 -> 显式切快速波形（WF_FAST）：收益明确
 *   - 脏区很大 -> 交驱动自己按帧差分选（WF_AUTO）：此时强制快速波形
 *     省不了多少时间，残影风险却一样，不划算
 * 纯函数语义——只读 p 的统计字段，不碰任何设备。 */
qzos_action_t qzos_policy_decide(qzos_policy_t *p, const qzos_panel_t *panel,
                                 int32_t dirty_pixels, int32_t total_pixels,
                                 qzos_waveform_t *out_wf);

/* 记录一次已完成的提交（动作真正落到屏上后调用）。
 * 写入失败时不要调用，策略会认为屏上还是旧内容。 */
void qzos_policy_on_committed(qzos_policy_t *p, qzos_action_t act);

const char *qzos_action_name(qzos_action_t a);

#endif /* QZOS_DISPLAY_POLICY_H */
