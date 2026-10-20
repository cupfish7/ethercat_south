/* ============================================================================
 *  ethercat_south_collector.c —— 南向模块的组装层
 *
 *  一拍的步骤
 *  ① DC 时：ecrt_master_application_time(target_ns)
 *  ② ecrt_master_receive()
 *  ③ ecrt_domain_process()
 *  ④ 查工作计数器 wc_state
 *  ⑤ 只有到点的从站才 read
 *  ⑥ DC 时：sync_reference_clock + sync_slave_clocks
 *  ⑦ ecrt_domain_queue() → ecrt_master_send()
 *  ⑧ 1 Hz 打一行汇总
 *
 *  DC 循环用**目标唤醒时刻**作为 application time（与 IgH dc_user 示例一致），
 *  而不是醒来后的实测时刻，避免抖动被写进参考时钟。
 * ============================================================================ */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <signal.h>

#include "ethercat_south_collector.h"
#include "ecrt.h"

#ifdef __linux__
#include <sys/mman.h>      /* mlockall */
#endif

#define DC_PERIOD_NS 1000000u   /* 主站周期固定 1 ms */

/* ---------- 停止请求 ---------- */

static volatile sig_atomic_t g_stop = 0;

static void collector_on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

void ethercat_south_collector_request_stop(void)
{
    g_stop = 1;
}

/* ---------- 时钟与睡眠 ---------- */

uint64_t ethercat_south_collector_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t ethercat_south_collector_now_ms(void)
{
    return ethercat_south_collector_now_ns() / 1000000ULL;
}

static void ms_to_timespec(uint64_t ms, struct timespec *ts)
{
    ts->tv_sec  = (time_t)(ms / 1000u);
    ts->tv_nsec = (long)((ms % 1000u) * 1000000u);
}

static void ns_to_timespec(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec  = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

/* 睡到指定的单调时刻。被信号打断（EINTR）时：
   无关信号继续睡，退出信号立刻返回（靠粘性 g_stop）。 */
static int sleep_until_ms(uint64_t ms)
{
    struct timespec target;
    int ret;

    ms_to_timespec(ms, &target);
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
    } while (ret == EINTR && !g_stop);

    return ret;
}

static int sleep_until_ns(uint64_t ns)
{
    struct timespec target;
    int ret;

    ns_to_timespec(ns, &target);
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
    } while (ret == EINTR && !g_stop);

    return ret;
}

/* ---------- 周期最大公约数（非 DC 调试模式定拍用） ---------- */

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static uint64_t slaves_gcd(const ethercat_south_collector_t *c)
{
    uint64_t g = 0;
    int i;

    for (i = 0; i < c->config->slave_count; i++) {
        uint64_t p = (uint64_t)c->config->slaves[i].period_ms;
        if (p == 0) continue;
        g = (g == 0) ? p : gcd_u64(g, p);
    }
    return g;
}

/* ---------- 实时设置 ---------- */

static void collector_setup_realtime(void)
{
    struct sched_param param;

    memset(&param, 0, sizeof(param));
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
        printf("警告：sched_setscheduler 失败（需要 root 权限？），以普通优先级运行\n");

#ifdef __linux__
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
        printf("警告：mlockall 失败，内存未锁定\n");
#endif
}

/* ---------- 启动 ---------- */

int ethercat_south_collector_start(ethercat_south_collector_t *c,
                                   const Config *config)
{
    int i;

    if (c == NULL || config == NULL) return -1;

    memset(c, 0, sizeof(*c));
    c->config = config;

    /* 先扫一遍配置：决定循环模式，并校验 DC 周期与固定 1 ms 主站一致。 */
    for (i = 0; i < config->slave_count; i++) {
        const SlaveConfig *s = &config->slaves[i];

        if (strcmp(s->protocol, "ethercat") != 0) continue;

        if (s->mode == SOUTH_MODE_DC) {
            if (s->dc.sync0_cycle_ns != DC_PERIOD_NS) {
                printf("[%s] DC 周期 %u ns 与固定 1 ms 主站不符\n",
                       s->node, s->dc.sync0_cycle_ns);
                return -1;
            }
            c->dc_enabled = 1;
            c->dc_period_ns = DC_PERIOD_NS;
        }
    }

    if (ethercat_south_master_start(config, &c->conn) != 0) {
        printf("主站启动失败\n");
        return -1;
    }

    c->base_ms        = ethercat_south_collector_now_ms();
    c->last_report_ms = c->base_ms;

    for (i = 0; i < config->slave_count; i++) {
        int p = config->slaves[i].period_ms;
        c->next_due_ms[i] = c->base_ms + (uint64_t)((p > 0) ? p : 1);
    }

    collector_setup_realtime();

    printf("collector 就绪：%s，%d 个从站，域 %lu 字节\n",
           c->dc_enabled ? "DC 1 ms" : "按从站周期",
           config->slave_count, (unsigned long)c->conn.domain_size);

    return 0;
}

/* ---------- 一拍 ---------- */

int ethercat_south_collector_tick_ns(ethercat_south_collector_t *c,
                                     uint64_t now_ns)
{
    uint64_t now_ms;
    int i;

    if (c == NULL || c->config == NULL) return -1;
    if (c->conn.master == NULL) return -1;

    now_ms = now_ns / 1000000ULL;

    /* ① DC 应用时间：用目标时刻，不用实测时刻。 */
    if (c->dc_enabled)
        ecrt_master_application_time(c->conn.master, now_ns);

    /* ② 收 */
    ecrt_master_receive(c->conn.master);

    /* ③ process */
    ecrt_domain_process(c->conn.domain);

    /* ④ 工作计数器 */
    {
        ec_domain_state_t ds;

        if (ecrt_domain_state(c->conn.domain, &ds) == 0 &&
            ds.wc_state != EC_WC_COMPLETE)
            c->wc_bad_count++;
    }

    /* ⑤ 只有到点的从站才 read */
    for (i = 0; i < c->config->slave_count; i++) {
        int period;

        if (now_ms < c->next_due_ms[i]) continue;
        if (c->conn.domain_pd == NULL) continue;

        period = c->config->slaves[i].period_ms;
        if (period <= 0) continue;

        if (ethercat_south_read_node(&c->conn, i, now_ms, &c->frame) == 0)
            c->read_count[i]++;

        /* 落后超过一整拍就跳过去对齐，不补读。 */
        c->next_due_ms[i] += (uint64_t)period;
        while (c->next_due_ms[i] <= now_ms)
            c->next_due_ms[i] += (uint64_t)period;
    }

    /* ⑥ DC 同步：reference clock 使用上一步写入的 application time。 */
    if (c->dc_enabled) {
        if (ecrt_master_sync_reference_clock(c->conn.master) != 0)
            c->dc_err_count++;
        if (ecrt_master_sync_slave_clocks(c->conn.master) != 0)
            c->dc_err_count++;
    }

    /* ⑦ queue + send */
    ecrt_domain_queue(c->conn.domain);
    ecrt_master_send(c->conn.master);

    c->tick_count++;

    /* ⑧ 1 Hz 汇总 */
    if (now_ms - c->last_report_ms >= 1000u) {
        c->last_report_ms = now_ms;
        printf("[collector] 运行 %lu s | 拍数 %lu | 模式 %s |",
               (unsigned long)((now_ms - c->base_ms) / 1000u),
               (unsigned long)c->tick_count,
               c->dc_enabled ? "DC 1ms" : "period");
        for (i = 0; i < c->config->slave_count; i++)
            printf(" %s=%lu帧", c->config->slaves[i].node,
                   (unsigned long)c->read_count[i]);
        if (c->wc_bad_count != 0)
            printf(" | !! 工作计数器异常 %lu 拍",
                   (unsigned long)c->wc_bad_count);
        if (c->dc_err_count != 0)
            printf(" | !! DC 同步失败 %lu 次",
                   (unsigned long)c->dc_err_count);
        printf("\n");
    }

    return 0;
}

int ethercat_south_collector_tick(ethercat_south_collector_t *c,
                                  uint64_t now_ms)
{
    return ethercat_south_collector_tick_ns(c, now_ms * 1000000ULL);
}

/* ---------- 两个 run ---------- */

int ethercat_south_collector_run(ethercat_south_collector_t *c)
{
    if (c == NULL) return -1;

    signal(SIGINT,  collector_on_signal);
    signal(SIGTERM, collector_on_signal);

    if (c->dc_enabled) {
        /* 固定 1 ms 绝对时刻唤醒。 */
        uint64_t next_ns = ethercat_south_collector_now_ns() + c->dc_period_ns;

        while (!g_stop) {
            int rc = sleep_until_ns(next_ns);

            if (g_stop) break;
            if (rc != 0) return -1;

            if (ethercat_south_collector_tick_ns(c, next_ns) != 0)
                return -1;

            next_ns += c->dc_period_ns;

            /* 落后一整拍以上直接对齐到现在，不连续补拍。 */
            {
                uint64_t actual = ethercat_south_collector_now_ns();
                if (next_ns <= actual)
                    next_ns = actual + c->dc_period_ns;
            }
        }

        return 0;
    }

    /* 非 DC：睡到最近一个从站到点。 */
    while (!g_stop) {
        uint64_t nearest = 0;
        int i, rc;

        for (i = 0; i < c->config->slave_count; i++) {
            if (nearest == 0 || c->next_due_ms[i] < nearest)
                nearest = c->next_due_ms[i];
        }
        if (nearest == 0) return -1;

        rc = sleep_until_ms(nearest);
        if (g_stop) break;
        if (rc != 0) return -1;

        if (ethercat_south_collector_tick(c,
                ethercat_south_collector_now_ms()) != 0)
            return -1;
    }

    return 0;
}

int ethercat_south_collector_run_debug(ethercat_south_collector_t *c,
                                       uint64_t tick_ms)
{
    if (c == NULL) return -1;

    signal(SIGINT,  collector_on_signal);
    signal(SIGTERM, collector_on_signal);

    if (c->dc_enabled) {
        /* DC 调试也必须是 1 ms；tick_ms 在这里不生效。 */
        uint64_t next_ns = ethercat_south_collector_now_ns() + c->dc_period_ns;

        while (!g_stop) {
            int rc = sleep_until_ns(next_ns);

            if (g_stop) break;
            if (rc != 0) return -1;

            if (ethercat_south_collector_tick_ns(c, next_ns) != 0)
                return -1;

            next_ns += c->dc_period_ns;
            {
                uint64_t actual = ethercat_south_collector_now_ns();
                if (next_ns <= actual)
                    next_ns = actual + c->dc_period_ns;
            }
        }

        return 0;
    }

    {
        uint64_t step = (tick_ms != 0) ? tick_ms : slaves_gcd(c);
        uint64_t next;

        if (step == 0) return -1;
        next = c->base_ms + step;

        while (!g_stop) {
            uint64_t now;
            int rc;

            rc = sleep_until_ms(next);
            if (g_stop) break;
            if (rc != 0) return -1;

            if (ethercat_south_collector_tick(c,
                    ethercat_south_collector_now_ms()) != 0)
                return -1;

            next += step;
            now = ethercat_south_collector_now_ms();
            if (next <= now)
                next = now + step;
        }
    }

    return 0;
}

/* ---------- 收尾 ---------- */

void ethercat_south_collector_stop(ethercat_south_collector_t *c)
{
    if (c == NULL || c->conn.master == NULL) return;

    /* 当前不写任何输出，没有需要先发下去的停机控制字。 */
    ecrt_release_master(c->conn.master);
    c->conn.master = NULL;
    c->conn.domain = NULL;
    c->conn.domain_pd = NULL;
    c->conn.domain_size = 0;

    printf("[collector] 已释放主站\n");
}
