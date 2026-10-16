#ifndef ETHERCAT_SOUTH_L6N_CONTROL_H
#define ETHERCAT_SOUTH_L6N_CONTROL_H

#include "ecrt.h"

/* ============================================================================
 *  L6N（CiA402 伺服）控制模块
 *
 *  移植自 aiworkplace/SUHUAN/ethercat_l6n_working.c，但按**本总线上真实的 L6N** 改过：
 *
 *    · vendor/product 不写死 —— 从站配置由 connect 按 JSON 里的值（0x00004321/
 *      0x000000e2）建好，本模块只接手，不重复建。参考实现里的
 *      0x000000dd/0x00007595 是另一个设备（注释写 InovanceAutomation），对不上。
 *
 *    · PDO 条目只注册**默认映射里真有的**：
 *          写：0x6040 控制字、0x607a 目标位置
 *          读：0x6041 状态字、0x6064 实际位置、0x6061 模式显示
 *      参考实现里的 0x6060（运行模式）和 0x6077（实际力矩）**不在**本从站的默认映射里
 *      （实测 SM2/0x1600 = 6040/607a/60b8，SM3/0x1a00 = 603f/6041/6061/6064/60b9/60ba/60fd）。
 *      所以：**0x6060 改走 SDO 设置**（那是唯一的办法），0x6077 不采集。
 *
 *    · 补了有序停机（_shutdown）—— 参考实现缺这一环，Ctrl+C 是直接 release master 的。
 *
 *  ⚠️ 本模块会让电机**真的转**：状态机走到 Operation enabled 之后会写目标位置。
 * ============================================================================ */

/* 运动指令的默认参数。
 *
 * 幅值按「22 位编码器 = 4,194,304 counts/转」折算：
 *     1048576 counts = 1/4 转 = ±90°，也就是**峰峰值 180° 的摆动**。
 *
 * 位数的来历：先按 20 位试过 262144 counts，实测量出来的摆角比 ±90° 小得多，
 *   反推出 counts/转 大约在 4.19M 量级 → 22 位。
 *   ⚠️ 这仍是估值。要精确应该读 0x608F（位置编码器分辨率：sub1 编码器增量 / sub2 电机转数）。
 *   早先的 50000 counts 肉眼几乎看不见，"电机没在转"的误判就是从那儿来的。
 *
 * 频率 0.5 Hz 照参考实现。 */
#define ETHERCAT_SOUTH_L6N_AMP_COUNTS   1048576.0   /* 正弦幅值 [编码器单位] ≈ ±90°（22 位） */
#define ETHERCAT_SOUTH_L6N_FREQ_HZ      0.5         /* 正弦频率 [Hz] */

/* CiA402 状态机的阶段 */
typedef enum {
    L6N_PHASE_FAULT_RESET = 0,  /* 驱动器报 Fault（状态字 bit3）→ 写 0x0080 复位 */
    L6N_PHASE_WAIT_MODE,        /* 等运行模式确认为 CSP(8) */
    L6N_PHASE_SHUTDOWN,         /* 控制字 0x0006 → Ready to switch on (0x21) */
    L6N_PHASE_SWITCH_ON,        /* 控制字 0x0007 → Switched on (0x23) */
    L6N_PHASE_ENABLE_OP,        /* 控制字 0x000F → Operation enabled (0x27) */
    L6N_PHASE_RUN               /* 0x001F + 正弦位置指令 */
} ethercat_south_l6n_phase_t;

typedef struct {
    /* ---- 由 _reg_pdo 填好 ---- */
    unsigned int off_control_word;      /* 0x6040 写 */
    unsigned int off_target_position;   /* 0x607a 写 */
    unsigned int off_status_word;       /* 0x6041 读 */
    unsigned int off_position_actual;   /* 0x6064 读 */
    unsigned int off_mode_display;      /* 0x6061 读 */
    unsigned int off_error_code;        /* 0x603f 读（CiA402 错误码；它本来就在 SM3 里，不占新字节） */
    int          registered;            /* 1 = 上面五个偏移都拿到了 */

    /* ---- CiA402 状态机 ---- */
    ethercat_south_l6n_phase_t phase;
    uint64_t phase_entered_ms;          /* 进入当前阶段的时刻（超时判定用） */
    uint64_t motion_start_ms;           /* 进入运行阶段的时刻（正弦的时间基准） */
    uint64_t last_print_ms;             /* 打印限流 */
    int      reached_op_enabled;        /* 是否到过 Operation enabled（诊断用） */
    int      faulted;                   /* 状态机超时 / 掉出使能过 */
    uint16_t last_status;               /* 上一拍的状态字（变化时打印） */

    /* 进入运行阶段后，实际位置的极值 —— "电机到底动没动"最直接的证据。
       真在跟随 ±amp_counts 的正弦，极值就该接近 ±amp_counts。 */
    int32_t  pos_min;
    int32_t  pos_max;
    uint64_t last_range_ms;             /* 上次打印极值范围的时刻 */

    /* ---- 运动参数 ---- */
    double amp_counts;                  /* 正弦幅值 [编码器单位] */
    double freq_hz;                     /* 正弦频率 [Hz] */
} ethercat_south_l6n_t;

/* 【配置期】activate 之前调。
   把运行模式设成 CSP(8)。本从站的默认 RxPDO 里没有 0x6060，所以只能走 SDO ——
   主站会在配置从站时把它下下去，通常就在 activate 那一下。 */
void ethercat_south_l6n_setup(ec_slave_config_t *sc, ethercat_south_l6n_t *l6n,
                              double amp_counts, double freq_hz);

/* 【配置期】activate 之前调。
   注册本模块要用的写/读条目，把偏移拿回来。返回 0 成功；-1 失败（该从站不做控制）。 */
int ethercat_south_l6n_reg_pdo(ec_slave_config_t *sc, ec_domain_t *domain,
                               ethercat_south_l6n_t *l6n);

/* 【周期期】每拍调一次（在该从站所属域"到点"的那一拍）。
   pd 是该域的过程映像基址，now_ms 是单调毫秒。
   做两件事：读状态字 → 跑状态机 → 写控制字（走到运行阶段还会写目标位置）。 */
void ethercat_south_l6n_step(ethercat_south_l6n_t *l6n, uint8_t *pd, uint64_t now_ms);

/* 【停机】把控制字打回 disable，让驱动器退出 Operation enabled。
   ⚠️ 只写进过程映像是不够的 —— 调用方还必须**再跑几拍**（queue + send），
   那个控制字才真的发到驱动器。参考实现连这一步都没有，是直接 release master 的。 */
void ethercat_south_l6n_shutdown(ethercat_south_l6n_t *l6n, uint8_t *pd);

#endif /* ETHERCAT_SOUTH_L6N_CONTROL_H */
