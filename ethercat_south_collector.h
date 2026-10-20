#ifndef ETHERCAT_SOUTH_COLLECTOR_H
#define ETHERCAT_SOUTH_COLLECTOR_H

#include "ethercat_south_read.h"       /* 连带 connect.h / parse.h / ecrt.h */

/* 南向模块的主库：把 parse / connect / read 组装起来。
 *
 * 组装关系：
 *     parse    JSON → Config
 *     connect  Config → conn
 *     read     conn + 从站下标 → 一帧
 *     collector（本模块）              串起上面三个 + 周期调度
 *
 * 调度：
 *   - 任一从站 mode==dc  → 固定 1 ms 循环，每拍设置 application time 并做 DC 同步；
 *   - 全部 freerun/sm    → 睡到最近一个从站到点的 ms 调度。
 *   period_ms 始终是**采样周期**（read 的周期），DC 周期由 dc.sync0_cycle_ns 决定。
 *
 * 时间单位：ms 与 ns 都基于 CLOCK_MONOTONIC，基准与 now_ms()/now_ns() 相同。 */

/* 采集器状态。调用方持有（约 40 KB），不能放栈上。 */
typedef struct {
    const Config *config;
    ethercat_south_conn_t conn;

    /* ---- 调度状态 ---- */
    uint64_t base_ms;                        /* 启动时刻（单调 ms），仅用于诊断 */
    uint64_t next_due_ms[MAX_SLAVES];        /* 各从站的下次到点时刻（单调 ms） */
    uint64_t tick_count;
    uint64_t read_count[MAX_SLAVES];
    uint64_t wc_bad_count;
    uint64_t last_report_ms;

    /* ---- DC ---- */
    int       dc_enabled;                    /* 任一从站 mode==dc */
    uint32_t  dc_period_ns;                  /* 固定 1000000 */
    uint64_t  dc_err_count;                  /* sync_reference/sync_slave 失败次数 */

    south_frame_t frame;                     /* 当前帧暂存；本轮不推送，读完即被覆盖 */
} ethercat_south_collector_t;

/* 组装并启动。需要 root（request_master）。
   返回 0 成功；-1 失败。 */
int ethercat_south_collector_start(ethercat_south_collector_t *c,
                                   const Config *config);

/* 跑一拍（ms 精度，非 DC / 手工循环用）。
   内部按 now_ns = now_ms * 1e6 调 tick_ns。 */
int ethercat_south_collector_tick(ethercat_south_collector_t *c,
                                  uint64_t now_ms);

/* 跑一拍（ns 精度）。DC 模式下会用 now_ns 设置 application time，
   并在 queue/send 前做参考时钟与从站时钟同步。 */
int ethercat_south_collector_tick_ns(ethercat_south_collector_t *c,
                                     uint64_t now_ns);

/* 【实际运行时接口】死循环。
   DC：固定 1 ms 绝对时刻唤醒；
   非 DC：睡到最近一个从站到点。
   Ctrl+C / SIGTERM 会让它返回 0。退出后必须调 _stop()。 */
int ethercat_south_collector_run(ethercat_south_collector_t *c);

/* 【调试接口】固定步长轮询。
   DC 模式固定 1 ms（忽略 tick_ms）；非 DC 模式 tick_ms=0 时取各从站周期 gcd。 */
int ethercat_south_collector_run_debug(ethercat_south_collector_t *c,
                                       uint64_t tick_ms);

/* 单调时钟。 */
uint64_t ethercat_south_collector_now_ms(void);
uint64_t ethercat_south_collector_now_ns(void);

/* 请求停止：粘性标志，异步信号安全。 */
void ethercat_south_collector_request_stop(void);

/* 收尾并释放主站。**必须在循环退出之后调**；幂等。
   当前不写任何输出，所以不需要停机刷拍。 */
void ethercat_south_collector_stop(ethercat_south_collector_t *c);

#endif /* ETHERCAT_SOUTH_COLLECTOR_H */
