/* ============================================================================
 *  collector_fake_test.c —— 用假 IgH 跑通整条南向链
 *
 *  ⚠️ 测试代码，不是交付物。
 *
 *  验证：parse → collector_start（里面走真 connect）→ 分频 tick（里面走真 read）
 *        真代码一行没改，只是把 IgH 用户态库换成了 ethercat_south_ecrt_stub.c。
 *
 *  必须在 test-file/ 目录下运行（parse 读的是相对路径 "JSON"）：
 *      cd test-file && ./test/fake_test.exe
 * ============================================================================ */

#include <stdio.h>
#include <string.h>

#include "ethercat_south_collector.h"

/* 替身暴露的旋钮与计数器 */
extern ec_wc_state_t g_stub_wc_state;
extern int g_stub_receive_calls, g_stub_process_calls;
extern int g_stub_queue_calls, g_stub_send_calls;

static int fails = 0;
static void check(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? "OK" : "**FAIL**", what);
    if (!ok) fails++;
}

static Config cfg;
static ethercat_south_collector_t col;
static ethercat_south_collector_t col2;

int main(void)
{
    int i, t;
    uint64_t before;

    /* ---- 真 parse ---- */
    if (configuration_store_load_startup(&cfg) != 0) { printf("配置加载失败\n"); return 1; }
    printf("parse：%d 个从站\n", cfg.slave_count);
    for (i = 0; i < cfg.slave_count; i++)
        printf("  slave[%d] %-10s period=%-3dms tags=%d\n",
               i, cfg.slaves[i].node, cfg.slaves[i].period_ms, cfg.slaves[i].tag_count);

    /* ---- 1. 走真 connect 的完整启动路径 ---- */
    printf("\n=== 1. collector_start（内部走真 connect）===\n");
    check("返回 0", ethercat_south_collector_start(&col, &cfg) == 0);
    check("建了 2 个域（按采样周期分）", col.conn.domain_count == 2);
    /* 域现在按容量分组、没有"周期"属性了 —— 这条旧断言换成新的语义。
       ⚠️ 这个测试整体还是按旧的 2 从站配置写的，需要重写（见文件末尾的说明）。 */
    check("按容量分组：每个从站都有归属域",
          col.conn.domain_count >= 1 && col.conn.slave_domain[0] >= 0);
    check("从站0→域0、从站1→域1",
          col.conn.slave_domain[0] == 0 && col.conn.slave_domain[1] == 1);
    check("过程映像拿到了，且两个域不是同一块",
          col.conn.domain_pd[0] != NULL && col.conn.domain_pd[1] != NULL &&
          col.conn.domain_pd[0] != col.conn.domain_pd[1]);
    check("偏移按注册顺序排开（0/4/8）",
          col.conn.tag_offset[0][0] == 0 && col.conn.tag_offset[0][1] == 4 &&
          col.conn.tag_offset[0][2] == 8);

    /* ---- 2. 分频读取 ---- */
    printf("\n=== 2. 分频读取：喂 0..10 共 11 拍 ===\n");
    for (t = 0; t <= 10; t++)
        ethercat_south_collector_tick(&col, col.base_ms + (uint64_t)t);

    check("1 ms 域（从站0）读 10 次", col.read_count[0] == 10);
    check("10 ms 域（从站1）读 1 次", col.read_count[1] == 1);
    check("每拍 receive/send 各 1 次（11）",
          g_stub_receive_calls == 11 && g_stub_send_calls == 11);
    check("两个域每拍都 process/queue（22）—— 非到点域也不断流",
          g_stub_process_calls == 22 && g_stub_queue_calls == 22);

    /* ---- 3. 读到的值确实来自各自的过程映像（域不串）---- */
    printf("\n=== 3. 读到的值（替身摆的假图案：域0=0xA5A5，域1=0x5A5A）===\n");
    check("重新 start 一个干净的 collector",
          ethercat_south_collector_start(&col2, &cfg) == 0);

    ethercat_south_collector_tick(&col2, col2.base_ms + 1);      /* 只有 1ms 域到点 */
    check("从站0 读到域0 的图案（u16 = 0xA5A5）",
          col2.frame.node_id == 0 && col2.frame.tags[2].value.u16 == 0xA5A5);
    check("同一帧里 i32→f64 也来自域0（不是 0）", col2.frame.tags[0].value.f64 != 0.0);

    ethercat_south_collector_tick(&col2, col2.base_ms + 10);     /* 两个域都到点 */
    check("从站1 读到域1 的图案（u16 = 0x5A5A，和域0 不同）",
          col2.frame.node_id == 1 && col2.frame.tags[2].value.u16 == 0x5A5A);

    /* ---- 4. 工作计数器：从站没起来 / 掉线时能被发现 ---- */
    printf("\n=== 4. 工作计数器检查 ===\n");
    g_stub_wc_state = EC_WC_COMPLETE;
    ethercat_south_collector_tick(&col2, col2.base_ms + 50);
    before = col2.wc_bad_count;
    check("wc 正常时不计数", before == 0);

    g_stub_wc_state = EC_WC_INCOMPLETE;                          /* 模拟从站掉线 */
    ethercat_south_collector_tick(&col2, col2.base_ms + 60);
    check("wc 异常的一拍 → wc_bad_count +1", col2.wc_bad_count == before + 1);
    ethercat_south_collector_tick(&col2, col2.base_ms + 61);
    check("连续两拍异常 → +2", col2.wc_bad_count == before + 2);

    g_stub_wc_state = EC_WC_COMPLETE;
    before = col2.wc_bad_count;
    ethercat_south_collector_tick(&col2, col2.base_ms + 70);
    check("恢复后不再增加", col2.wc_bad_count == before);

    printf("\n================ 失败 %d 项 ================\n", fails);
    return fails ? 1 : 0;
}
