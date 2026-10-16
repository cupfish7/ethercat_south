/* ============================================================================
 *  ethercat_south_l6n_control.c —— L6N（CiA402 伺服）控制
 *
 *  移植自 aiworkplace/SUHUAN/ethercat_l6n_working.c。改动点见 .h 的说明，摘要：
 *    · vendor/product 不写死（从站配置由 connect 按 JSON 建好）
 *    · PDO 条目只注册默认映射里真有的（0x6060 改走 SDO，0x6077 不采集）
 *    · 时间基准改成 now_ms，不再依赖"每拍正好 1 ms"
 *    · 补了有序停机
 *
 *  ⚠️ 会让电机真的转。
 * ============================================================================ */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ethercat_south_l6n_control.h"

/* ---------- CiA402 常量（照参考实现） ---------- */

#define CIA402_MASK            0x006F
#define CIA402_READY_SWITCH_ON 0x0021   /* Ready to switch on */
#define CIA402_SWITCHED_ON     0x0023   /* Switched on        */
#define CIA402_OP_ENABLED      0x0027   /* Operation enabled  */
#define CIA402_FAULT           0x0008   /* bit3 = Fault；必须先复位才能往下走 */

#define CW_DISABLE_VOLTAGE  0x0000
#define CW_SHUTDOWN         0x0006
#define CW_SWITCH_ON        0x0007
#define CW_ENABLE_OP        0x000F
#define CW_ENABLE_OP_NEW    0x001F      /* Enable Operation + New setpoint */
#define CW_FAULT_RESET      0x0080      /* bit7 = Fault reset */

#define MODE_CSP            8           /* 周期同步位置模式 */

/* 超时 [ms]。参考实现写的是"5000/3000 个 1kHz 周期"，这里换成毫秒，
   这样节拍不是 1 kHz 时语义也对。 */
#define TIMEOUT_MODE_MS     5000
#define TIMEOUT_STEP_MS     3000
/* 故障复位给得宽一点：故障可能是持续性的（真过流/过压），要给足时间看清错误码 */
#define TIMEOUT_FAULT_MS   10000

#define PRINT_EVERY_MS      1000
/* 运行阶段的打印间隔。**不能是 1000 ms** —— 正弦是 0.5 Hz（周期 2 s），
   按 1 s 采样正好落在每个周期的零点上，拿到的永远只有 ±A·sin(φ) 这一个交替序列，
   分不出"真在摆动"和"根本没动"。100 ms 每周期 20 个点，波形直接现形。 */
#define PRINT_MOTION_MS      100

/* ---------- 配置期 ---------- */

void ethercat_south_l6n_setup(ec_slave_config_t *sc, ethercat_south_l6n_t *l6n,
                              double amp_counts, double freq_hz)
{
    memset(l6n, 0, sizeof(*l6n));

    l6n->amp_counts = (amp_counts > 0.0) ? amp_counts : ETHERCAT_SOUTH_L6N_AMP_COUNTS;
    l6n->freq_hz    = (freq_hz    > 0.0) ? freq_hz    : ETHERCAT_SOUTH_L6N_FREQ_HZ;
    l6n->phase      = L6N_PHASE_WAIT_MODE;

    /* 运行模式设成 CSP(8)。
       参考实现是靠把 0x6060 映射进 RxPDO 再每拍写 —— 但那台从站（Inovance）的
       默认映射里有 0x6060，**本总线上这台 L6N 没有**，注册会失败。
       所以这里改走 SDO：主站在配置从站时下下去，通常就在 activate 那一下。 */
    if (ecrt_slave_config_sdo8(sc, 0x6060, 0x00, MODE_CSP) != 0)
        printf("[l6n] 警告：SDO 写 0x6060:00 = CSP(%d) 失败\n", MODE_CSP);
}

int ethercat_south_l6n_reg_pdo(ec_slave_config_t *sc, ec_domain_t *domain,
                               ethercat_south_l6n_t *l6n)
{
    int off;

    l6n->registered = 0;

#define REG(_idx, _dst, _what)                                          \
    do {                                                                \
        off = ecrt_slave_config_reg_pdo_entry(sc, (_idx), 0x00, domain, NULL); \
        if (off < 0) {                                                  \
            printf("[l6n] 注册 %s(0x%04X) 失败 —— 该从站不做控制\n", (_what), (_idx)); \
            return -1;                                                  \
        }                                                               \
        (_dst) = (unsigned int)off;                                     \
    } while (0)

    REG(0x6040, l6n->off_control_word,    "控制字");
    REG(0x607a, l6n->off_target_position, "目标位置");
    REG(0x6041, l6n->off_status_word,     "状态字");
    REG(0x6064, l6n->off_position_actual, "实际位置");
    REG(0x6061, l6n->off_mode_display,    "模式显示");
    /* 0x603f（错误码）本来就在 SM3 的 TxPDO 里，注册它不新增域字节，
       只是把偏移取回来 —— 但**没有它就只能看到"故障了"，看不到"什么故障"**。 */
    REG(0x603f, l6n->off_error_code,      "错误码");

#undef REG

    /* 注意：0x6064 很可能已经被采集那条路注册过（JSON 里的 position_actual）。
       ecrt_slave_config_reg_pdo_entry 对同一条目是幂等的，两次拿到同一个偏移，不会重复占位。 */

    l6n->registered = 1;
    return 0;
}

/* ---------- 周期期 ---------- */

void ethercat_south_l6n_step(ethercat_south_l6n_t *l6n, uint8_t *pd, uint64_t now_ms)
{
    uint16_t status;
    int8_t   mode_disp;
    double   t;
    int32_t  target;

    if (l6n == NULL || pd == NULL || !l6n->registered) return;

    /* 第一次进来时把"本阶段从什么时候开始"定下来。
       ⚠️ 这一行不能省：_setup() 里没有时钟可用，phase_entered_ms 是 memset 出来的 0；
       而 now_ms 是"开机以来的单调毫秒"（不是程序运行了多久），于是
       now_ms - 0 会立刻超过任何一个超时值 —— 阶段 0 第一拍就误判超时，
       状态机一步都走不了。这个坑已经踩过一次。
       用 0 当作"还没初始化"是安全的：单调毫秒只有在开机那一瞬间才是 0。 */
    if (l6n->phase_entered_ms == 0) l6n->phase_entered_ms = now_ms;

    /* 出过故障就一直压在 disable —— 要不要重试是上层的事，本模块不自动重试。
       理由：一个自己会重试的伺服使能序列，在现场是很难查的。 */
    if (l6n->faulted) {
        EC_WRITE_U16(pd + l6n->off_control_word, CW_DISABLE_VOLTAGE);
        return;
    }

    status    = EC_READ_U16(pd + l6n->off_status_word);
    mode_disp = EC_READ_S8 (pd + l6n->off_mode_display);

    /* ---- 故障守卫：CiA402 的 Fault 必须先复位，否则后面写什么都没用 ----
       放在 switch 之前，这样**任何阶段**（包括运行中）掉进故障都能抓到。
       参考实现完全没有这一步 —— 它跑的那台驱动器没报过故障。 */
    if (status & CIA402_FAULT) {
        if (l6n->phase != L6N_PHASE_FAULT_RESET) {
            uint16_t err = EC_READ_U16(pd + l6n->off_error_code);
            printf("[l6n] ⚠️ 驱动器报故障：状态字=0x%04X 错误码=0x%04X(%u) —— 写 Fault reset 尝试复位\n",
                   status, err, err);
            l6n->phase = L6N_PHASE_FAULT_RESET;
            l6n->phase_entered_ms = now_ms;
            l6n->last_print_ms = now_ms;
        }
    }

    switch (l6n->phase) {

    case L6N_PHASE_FAULT_RESET:
        /* 写 bit7 = Fault reset。驱动器清掉故障后会回到 Switch on disabled，
           之前走到哪一步都不作数了 —— 所以复位成功后**重走整条使能序列**。 */
        EC_WRITE_U16(pd + l6n->off_control_word, CW_FAULT_RESET);

        if (!(status & CIA402_FAULT)) {
            printf("[l6n] 故障已清除，重新走使能序列\n");
            l6n->phase = L6N_PHASE_SHUTDOWN;
            l6n->phase_entered_ms = now_ms;
        } else if (now_ms - l6n->phase_entered_ms > TIMEOUT_FAULT_MS) {
            printf("[l6n] 故障复位超时（错误码=0x%04X）—— 放弃\n",
                   EC_READ_U16(pd + l6n->off_error_code));
            l6n->faulted = 1;
        } else if (now_ms - l6n->last_print_ms >= PRINT_EVERY_MS) {
            printf("[l6n] 正在复位… 状态字=0x%04X 错误码=0x%04X\n",
                   status, EC_READ_U16(pd + l6n->off_error_code));
            l6n->last_print_ms = now_ms;
        }
        break;

    case L6N_PHASE_WAIT_MODE:
        /* 模式已经在 _setup 里用 SDO 写下去了，这里只等它反映到 mode display */
        EC_WRITE_U16(pd + l6n->off_control_word, CW_DISABLE_VOLTAGE);

        if (mode_disp == MODE_CSP) {
            printf("[l6n] 运行模式确认为 CSP(%d)，开始使能序列\n", MODE_CSP);
            l6n->phase = L6N_PHASE_SHUTDOWN;
            l6n->phase_entered_ms = now_ms;
        } else if (now_ms - l6n->phase_entered_ms > TIMEOUT_MODE_MS) {
            printf("[l6n] 设置运行模式超时（模式显示=%d，期望 %d）\n", mode_disp, MODE_CSP);
            l6n->faulted = 1;
        }
        break;

    case L6N_PHASE_SHUTDOWN:
        EC_WRITE_U16(pd + l6n->off_control_word, CW_SHUTDOWN);
        if ((status & CIA402_MASK) == CIA402_READY_SWITCH_ON) {
            printf("[l6n] Shutdown → Ready to switch on (0x%04X)\n", status);
            l6n->phase = L6N_PHASE_SWITCH_ON;
            l6n->phase_entered_ms = now_ms;
        } else if (now_ms - l6n->phase_entered_ms > TIMEOUT_STEP_MS) {
            printf("[l6n] Shutdown 超时（状态=0x%04X）\n", status);
            l6n->faulted = 1;
        }
        break;

    case L6N_PHASE_SWITCH_ON:
        EC_WRITE_U16(pd + l6n->off_control_word, CW_SWITCH_ON);
        if ((status & CIA402_MASK) == CIA402_SWITCHED_ON) {
            printf("[l6n] Switch On → Switched on (0x%04X)\n", status);
            l6n->phase = L6N_PHASE_ENABLE_OP;
            l6n->phase_entered_ms = now_ms;
        } else if (now_ms - l6n->phase_entered_ms > TIMEOUT_STEP_MS) {
            printf("[l6n] Switch On 超时（状态=0x%04X）\n", status);
            l6n->faulted = 1;
        }
        break;

    case L6N_PHASE_ENABLE_OP:
        EC_WRITE_U16(pd + l6n->off_control_word, CW_ENABLE_OP);
        if ((status & CIA402_MASK) == CIA402_OP_ENABLED) {
            printf("[l6n] Enable Operation → Operation enabled (0x%04X)\n", status);
            printf("[l6n] ⚠️ 开始正弦位置指令：%.2f Hz，±%.0f counts —— 电机即将运动\n",
                   l6n->freq_hz, l6n->amp_counts);
            l6n->phase = L6N_PHASE_RUN;
            l6n->phase_entered_ms  = now_ms;
            l6n->motion_start_ms   = now_ms;
            l6n->last_print_ms     = now_ms;
            l6n->last_range_ms     = now_ms;
            l6n->pos_min           = INT32_MAX;     /* 用极值判断"动没动"，先放到最宽 */
            l6n->pos_max           = INT32_MIN;
            l6n->reached_op_enabled = 1;
        } else if (now_ms - l6n->phase_entered_ms > TIMEOUT_STEP_MS) {
            printf("[l6n] Enable Operation 超时（状态=0x%04X）\n", status);
            l6n->faulted = 1;
        }
        break;

    case L6N_PHASE_RUN:
        t = (double)(now_ms - l6n->motion_start_ms) / 1000.0;
        target = (int32_t)(l6n->amp_counts * sin(2.0 * M_PI * l6n->freq_hz * t));

        EC_WRITE_S32(pd + l6n->off_target_position, target);
        EC_WRITE_U16(pd + l6n->off_control_word, CW_ENABLE_OP_NEW);

        /* 掉出使能：报警并停发运动指令（不自动重试） */
        if ((status & CIA402_MASK) != CIA402_OP_ENABLED) {
            printf("[l6n] ⚠️ 掉出 Operation enabled（状态=0x%04X）—— 停止运动指令\n", status);
            l6n->faulted = 1;
            break;
        }

        {
            int32_t pos = EC_READ_S32(pd + l6n->off_position_actual);

            /* 极值：真在跟随 ±amp_counts 的正弦，它就该接近 ±amp_counts */
            if (pos < l6n->pos_min) l6n->pos_min = pos;
            if (pos > l6n->pos_max) l6n->pos_max = pos;

            /* 100 ms 打一行 —— 每周期 20 个点，波形直接现形 */
            if (now_ms - l6n->last_print_ms >= PRINT_MOTION_MS) {
                printf("[l6n] t=%6.2fs 目标=%8d 实际=%8d 误差=%6d 状态=0x%04X\n",
                       t, target, pos, target - pos, status);
                l6n->last_print_ms = now_ms;
            }

            /* 每秒一行极值范围 —— 这是"电机到底动没动"的判决行 */
            if (now_ms - l6n->last_range_ms >= PRINT_EVERY_MS) {
                printf("[l6n] == t=%6.2fs 实际位置范围 [%d, %d]，跨度 %d（指令 ±%.0f）\n",
                       t, l6n->pos_min, l6n->pos_max,
                       l6n->pos_max - l6n->pos_min, l6n->amp_counts);
                l6n->last_range_ms = now_ms;
            }
        }
        break;
    }

    l6n->last_status = status;
}

/* ---------- 停机 ---------- */

void ethercat_south_l6n_shutdown(ethercat_south_l6n_t *l6n, uint8_t *pd)
{
    if (l6n == NULL || pd == NULL || !l6n->registered) return;

    /* 控制字打回 Shutdown —— 驱动器会退出 Operation enabled。
       注意这只写进了过程映像；调用方必须**再跑几拍**（queue + send）它才真的到驱动器。
       参考实现连这一步都没有，Ctrl+C 是直接 release master 的。 */
    EC_WRITE_U16(pd + l6n->off_control_word, CW_SHUTDOWN);
    l6n->phase = L6N_PHASE_SHUTDOWN;
    l6n->phase_entered_ms = 0;
    l6n->faulted = 1;                   /* 不再自动往下走 */
}
