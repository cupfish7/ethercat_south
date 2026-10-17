#ifndef ETHERCAT_SOUTH_COLLECTOR_H
#define ETHERCAT_SOUTH_COLLECTOR_H

#include "ethercat_south_read.h"       /* 连带 connect.h / parse.h / ecrt.h */

/* 南向模块的主库：把 parse / connect / read 组装起来。
 *
 * 组装关系：
 *     parse    JSON → Config            （通用工具，别的程序也能用）
 *     connect  Config → conn            （建主站、按采样周期建域、注册 PDO、激活）
 *     read     conn + 从站下标 → 一帧   （只取数，不驱动周期）
 *     collector（本模块）               （串起上面三个 + 按各域周期分频调 read）
 *
 * 时间单位统一：所有 "ms" 都是**单调毫秒**，基准与 ethercat_south_collector_now_ms() 相同。 */

/* 采集器状态。调用方持有（约 40 KB：conn 33 KB + 一帧暂存 5.7 KB + 调度状态）。
   不能放栈上。 */
typedef struct {
    const Config *config;                    /* 调用方持有，生命周期须长于本结构体 */
    ethercat_south_conn_t conn;              /* 域、过程映像、从站→域映射、tag 偏移 */

    /* ---- 分频调度状态 ---- */
    uint64_t base_ms;                        /* 启动时刻（单调 ms），仅用于诊断 */
    uint64_t next_due_ms[MAX_SLAVES];        /* 各**从站**的下次到点时刻（单调 ms，与 now_ms 同基准） */
    uint64_t tick_count;                     /* 跑了多少拍 */
    uint64_t read_count[MAX_SLAVES];         /* 每个从站累计读了多少帧 */
    uint64_t wc_bad_count;                   /* 有多少拍里至少有一个域的工作计数器不正常 */
    uint64_t last_report_ms;                 /* 上次打印汇总的时刻（1 Hz 限流用） */

    south_frame_t frame;                     /* 读出来的帧的暂存处。
                                                本轮不推送，读完即被下一帧覆盖 —— 这不是遗漏。 */
} ethercat_south_collector_t;

/* 组装并启动：建主站、按采样周期建域、注册 PDO 条目、激活、取过程映像，
   然后把调度状态初始化好。需要 root（要 request_master）。
   返回 0 成功；-1 失败。 */
int ethercat_south_collector_start(ethercat_south_collector_t *c, const Config *config);

/* 跑一拍：receive → 所有域 process → 到点的域逐从站 read → 所有域 queue → send。
   now_ms 由调用方给（单调毫秒）—— collector 自己不取时钟，保持可单测。
   返回 0 成功；-1 参数非法。单个帧读不到不影响整拍（仍返回 0）。 */
int ethercat_south_collector_tick(ethercat_south_collector_t *c, uint64_t now_ms);

/* 【实际运行时接口 —— 部署用这个】
   自己计时，睡到"最近一个域到点"才跑一拍。
   好处：CPU 占用最低，任意周期组合都不浪费。
   代价：拍间隔不规整（1ms 域和 10ms 域混着来），调试时不好对时间。
   死循环，只在 tick 出错时返回。要优雅退出请改用 _tick()。 */
int ethercat_south_collector_run(ethercat_south_collector_t *c);

/* 【调试接口】
   以各域周期的最大公约数定拍轮询，每拍都进 tick。
   好处：拍间隔固定、好观察好单步，打印输出好对时间。
   代价：若周期组合是 3ms+10ms 这种，gcd=1ms 会白跑很多拍。
   tick_ms 传 0 表示自动取各域周期的最大公约数。
   死循环，只在 tick 出错时返回。 */
int ethercat_south_collector_run_debug(ethercat_south_collector_t *c, uint64_t tick_ms);

/* 单调毫秒时钟（CLOCK_MONOTONIC）。
   给"调用方自己控循环"的用法配 _tick() 用：now_ms 直接传它的返回值。 */
uint64_t ethercat_south_collector_now_ms(void);

/* ---------------------------------------------------------------- 优雅退出 */

/* 请求停止：置一个**粘性**标志（置位后永不清零）。
   _run() / _run_debug() 的循环每轮看它，看到就退出。
   异步信号安全 —— 可以直接从信号处理器里调。
   也可以由调用方自己装处理器后调它。 */
void ethercat_south_collector_request_stop(void);

/* 收尾并释放主站。**必须在循环退出之后调**，返回后再调 _tick() 是安全的空操作。
   幂等：重复调用无害。 */
void ethercat_south_collector_stop(ethercat_south_collector_t *c);

#endif /* ETHERCAT_SOUTH_COLLECTOR_H */
