/* ============================================================================
 *  ethercat_south_collector.c —— 南向模块的组装层
 *
 *  职责：把 parse / connect / read 串起来，让项目主函数一行就能跑起来。
 *
 *  组装关系
 *  --------
 *      parse      JSON  →  Config           （通用工具，别的主站程序也能用）
 *      connect    Config →  conn            （建主站、按采样周期建域、注册 PDO、激活）
 *      read       conn + 从站下标 → 一帧    （只取数，不驱动周期）
 *      collector  本文件                    （串起上面三个 + 按各域周期分频调 read）
 *
 *  周期循环（两个接口，用途不同）
 *  ------------------------------
 *      _run()            睡到"最近一个域到点"才跑一拍 —— 【实际运行时用】
 *                        CPU 占用最低，代价是拍间隔不规整
 *      _run_debug()      以各域周期的最大公约数定拍轮询  —— 【调试用】
 *                        拍间隔固定好观察，代价是 gcd 小的时候会白跑
 *      _tick()           调用方自己给时间、自己控循环
 *
 *  一拍的步骤
 *  ----------
 *
 *    ① ecrt_master_receive()        收
 *    │     └ 紧接着打时间戳 —— 这是 read 要的 source_received_at_ms
 *    ② 所有域 ecrt_domain_process()  注意：不是只处理到点的域
 *    │                                非到点域的数据流也不能断（从站看门狗）
 *    ③ 查工作计数器 wc_state         —— 这是"从站没起来 / 掉线"的唯一防线
 *    │                                没有它，read 会照读一块全是 0 的过程映像，
 *    │                                下游看到的是一条完全健康的流水线
 *    ④ 只有到点的域才逐从站 read
 *    │     └ 读完的帧暂存在 c->frame
 *    │       TODO: 推送 —— 队列建好后在这里投给下游（本轮明确不做）
 *    ⑤ 所有域 ecrt_domain_queue() → ecrt_master_send()
 *    ⑥ 1 Hz 打一行汇总（本轮"不推送"，靠它证明真的在按周期读）
 *
 *  时间单位统一：所有 "ms" 都是单调毫秒，基准与 ethercat_south_collector_now_ms() 相同。
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
#include <sys/mman.h>      /* mlockall —— MinGW 没有这个声明，必须包住 */
#endif

/* ---------- 停止请求（优雅退出用） ---------- */

/* 粘性标志：置位后永不清零。被信号处理器写、被主循环读，
   所以必须是 volatile sig_atomic_t。

   **刻意不采用"记信号号、运行时再判断是不是退出信号"那种做法** —— 它有两个毛病：
     ① 后来的无关信号会把记录下来的信号号覆盖掉；
     ② 退出信号先到、无关信号紧随其后时会**丢掉退出请求**。
   粘性标志两个都不会犯：无关信号不碰它，退出请求一旦置位就不会被盖掉。

   信号身份的区分其实在**注册阶段**就完成了 —— 给谁装 collector_on_signal、
   给谁不装，那一行就定了，运行时不需要再识别。 */
static volatile sig_atomic_t g_stop = 0;

static void collector_on_signal(int sig)
{
    (void)sig;
    g_stop = 1;          /* 处理器里只能做异步信号安全的事：设标志。绝不能 printf。 */
}

void ethercat_south_collector_request_stop(void)
{
    g_stop = 1;
}

/* ---------- 时钟与睡眠 ---------- */

uint64_t ethercat_south_collector_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void ms_to_timespec(uint64_t ms, struct timespec *ts)
{
    ts->tv_sec  = (time_t)(ms / 1000u);
    ts->tv_nsec = (long)((ms % 1000u) * 1000000u);
}

/* 睡到指定的单调时刻。clock_nanosleep 直接返回错误码（不设 errno）。

   被信号打断（EINTR）时：**无关信号继续睡，退出信号立刻返回** ——
   靠的是粘性标志 g_stop：无关信号不碰它，循环条件仍然成立，于是接着睡；
   一旦有退出请求，条件立刻不成立，马上返给调用方。

   TIMER_ABSTIME 在这里是白捡的便利：目标是**绝对时刻**，重睡不用重算剩余时间，
   拿同一个 target 再调一次就是"继续睡到原来那个时刻"。
   （换成相对时长的 nanosleep 就得自己扣时间，还容易累积误差。） */
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

/* ---------- 周期最大公约数（调试模式定拍用） ---------- */

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* 各域周期的最大公约数。返回 0 表示一个有效周期都没有。 */
static uint64_t domains_gcd(const ethercat_south_collector_t *c)
{
    uint64_t g = 0;
    int k;

    for (k = 0; k < c->conn.domain_count; k++) {
        uint64_t p = (uint64_t)c->conn.domain_period[k];
        if (p == 0) continue;
        g = (g == 0) ? p : gcd_u64(g, p);
    }
    return g;
}

/* ---------- 实时设置 ---------- */

/* 照 examples/user/main.c:347-364 的做法。
   失败只告警不退出 —— 没有 root 时照样能跑，只是抖动大、更容易丢拍。 */
static void collector_setup_realtime(void)
{
    struct sched_param param;

    memset(&param, 0, sizeof(param));
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
        printf("警告：sched_setscheduler 失败（需要 root 权限？），以普通优先级运行\n");

#ifdef __linux__
    /* 锁内存，防止周期任务里发生缺页。
       MinGW 没有 mlockall 的声明 —— 这里包住，否则本机连语法检查都过不了。 */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
        printf("警告：mlockall 失败，内存未锁定\n");
#endif
}

/* ---------- 启动 ---------- */

int ethercat_south_collector_start(ethercat_south_collector_t *c, const Config *config)
{
    int k;

    if (c == NULL || config == NULL) return -1;

    memset(c, 0, sizeof(*c));
    c->config = config;

    /* 建主站 → 按采样周期建域 → 注册 PDO 条目 → 激活 → 取各自的过程映像。
       这一整段都在 connect 模块里，collector 不重复它的逻辑。 */
    if (ethercat_south_master_start(config, &c->conn) != 0) {
        printf("主站启动失败\n");
        return -1;
    }

    /* 调度状态初始化：每个域第一次到点 = 启动时刻 + 它自己的周期 */
    c->base_ms        = ethercat_south_collector_now_ms();
    c->last_report_ms = c->base_ms;
    for (k = 0; k < c->conn.domain_count; k++)
        c->next_due_ms[k] = c->base_ms + (uint64_t)c->conn.domain_period[k];

    collector_setup_realtime();

    printf("collector 就绪：%d 个域、%d 个从站\n",
           c->conn.domain_count, config->slave_count);
    for (k = 0; k < c->conn.domain_count; k++)
        printf("  域 %d: period %d ms\n", k, c->conn.domain_period[k]);

    return 0;
}

/* ---------- 一拍 ---------- */

int ethercat_south_collector_tick(ethercat_south_collector_t *c, uint64_t now_ms)
{
    uint64_t received_at;
    int i, k;

    if (c == NULL || c->config == NULL) return -1;
    if (c->conn.master == NULL) return -1;

    /* ① 收 —— 顺序照 IgH 示例固定：receive → process → (读) → queue → send */
    ecrt_master_receive(c->conn.master);
    received_at = now_ms;      /* 紧接 receive 打戳，这就是 read 要的 source_received_at_ms */

    /* ② 所有域都 process（不是只处理到点的）—— 非到点域的数据流也不能断 */
    for (k = 0; k < c->conn.domain_count; k++)
        ecrt_domain_process(c->conn.domains[k]);

    /* ③ 查工作计数器 —— 这是"从站没起来 / 掉线"的**唯一防线**。
          没有它，过程映像里可能全是 0 或上次的残值，而 read 照读不误、帧序列照涨，
          下游拿到一条看起来完全健康的流水线。ecrt_domain_state 必须在
          domain_process 之后调才有意义（它读的是上一轮交换的结果）。 */
    {
        int bad = 0;

        for (k = 0; k < c->conn.domain_count; k++) {
            ec_domain_state_t ds;

            if (ecrt_domain_state(c->conn.domains[k], &ds) != 0) continue;
            if (ds.wc_state != EC_WC_COMPLETE) bad = 1;
        }
        if (bad) c->wc_bad_count++;      /* 按"拍"计：这一拍至少有一个域不正常 */
    }

    /* ④ 只有到点的域才 read */
    for (k = 0; k < c->conn.domain_count; k++) {
        int period;

        if (now_ms < c->next_due_ms[k]) continue;          /* 没到点，跳过 */

        period = c->conn.domain_period[k];
        if (period <= 0) continue;                         /* 周期无效，读不了也不排下一拍 */

        for (i = 0; i < c->config->slave_count; i++) {
            if (c->conn.slave_domain[i] != k) continue;    /* 这个从站不属于本域 */

            if (ethercat_south_read_node(&c->conn, i, received_at, &c->frame) == 0) {
                c->read_count[i]++;
                /* TODO: 推送 —— 队列建好后在这里把 &c->frame 投给下游。
                   本轮只到"周期读取"，读完的帧下一拍就被覆盖。 */
            }
        }

        /* 排下一拍。落后超过一整拍就跳过去对齐，**不补读** ——
           一次停顿若引发一串补读，会越滚越糟；宁可丢拍也要跟上节奏。 */
        c->next_due_ms[k] += (uint64_t)period;
        while (c->next_due_ms[k] <= now_ms)
            c->next_due_ms[k] += (uint64_t)period;
    }

    /* ⑤ 所有域都 queue，再 send */
    for (k = 0; k < c->conn.domain_count; k++)
        ecrt_domain_queue(c->conn.domains[k]);
    ecrt_master_send(c->conn.master);

    c->tick_count++;

    /* ⑥ 1 Hz 打一行汇总 —— 本轮"不推送"，靠它证明真的在按周期读 */
    if (now_ms - c->last_report_ms >= 1000u) {
        c->last_report_ms = now_ms;
        printf("[collector] 运行 %lu s | 拍数 %lu |",
               (unsigned long)((now_ms - c->base_ms) / 1000u),
               (unsigned long)c->tick_count);
        for (i = 0; i < c->config->slave_count; i++)
            printf(" %s=%lu帧", c->config->slaves[i].node,
                   (unsigned long)c->read_count[i]);
        if (c->wc_bad_count != 0)
            printf(" | !! 工作计数器异常 %lu 拍（从站没起来或掉线？读到的可能是空气）",
                   (unsigned long)c->wc_bad_count);
        printf("\n");
    }

    return 0;
}

/* ---------- 两个 run ---------- */

/* 【实际运行时】睡到"所有域里最近的一个到点"，醒了一拍，再算下一个最近点。

   Ctrl+C / SIGTERM 会让它退出（返回 0）。**退出后必须调 _stop() 收尾**（释放主站）。 */
int ethercat_south_collector_run(ethercat_south_collector_t *c)
{
    if (c == NULL) return -1;

    signal(SIGINT,  collector_on_signal);
    signal(SIGTERM, collector_on_signal);

    while (!g_stop) {
        uint64_t nearest = 0;
        int k, rc;

        for (k = 0; k < c->conn.domain_count; k++) {
            if (nearest == 0 || c->next_due_ms[k] < nearest)
                nearest = c->next_due_ms[k];
        }
        if (nearest == 0) return -1;              /* 一个域都没有，没什么可跑的 */

        rc = sleep_until_ms(nearest);
        if (g_stop) break;                        /* 因退出请求而返回：不是错误 */
        if (rc != 0) return -1;

        if (ethercat_south_collector_tick(c, ethercat_south_collector_now_ms()) != 0)
            return -1;
    }

    return 0;
}

/* 【调试】以固定步长定拍轮询：不管有没有域到点，每拍都进 tick。 */
int ethercat_south_collector_run_debug(ethercat_south_collector_t *c, uint64_t tick_ms)
{
    uint64_t step, next;

    if (c == NULL) return -1;

    step = (tick_ms != 0) ? tick_ms : domains_gcd(c);
    if (step == 0) return -1;

    signal(SIGINT,  collector_on_signal);
    signal(SIGTERM, collector_on_signal);

    next = c->base_ms + step;

    while (!g_stop) {
        uint64_t now;
        int rc;

        rc = sleep_until_ms(next);
        if (g_stop) break;                        /* 因退出请求而返回：不是错误 */
        if (rc != 0) return -1;

        if (ethercat_south_collector_tick(c, ethercat_south_collector_now_ms()) != 0)
            return -1;

        /* 定拍：用绝对时刻累加，不重新取当前时间，避免累积漂移。
           但落后一整拍以上时直接对齐到现在 —— 否则会连续忙转补拍。 */
        next += step;
        now = ethercat_south_collector_now_ms();
        if (next <= now)
            next = now + step;
    }

    return 0;
}

/* ---------- 优雅退出：收尾 ---------- */

void ethercat_south_collector_stop(ethercat_south_collector_t *c)
{
    if (c == NULL || c->conn.master == NULL) return;

    /* 这一版没有电机控制 —— 应用从不往过程映像里写输出，所以释放主站前
       没有什么"要先发下去"的东西。直接释放即可：从站会回 PREOP。
       （带伺服控制的那一版在这里还要先发 disable 控制字、再跑几拍把它发出去、
         然后等驱动器退出使能 —— 顺序不能省，这里不需要。） */
    ecrt_release_master(c->conn.master);
    c->conn.master = NULL;          /* 置 NULL：之后 _tick() 变成安全空操作 */

    printf("[collector] 已释放主站\n");
}
