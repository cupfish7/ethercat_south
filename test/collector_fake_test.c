/* ============================================================================
 *  collector_fake_test.c —— 用假 IgH 跑通整条南向链
 *
 *  ⚠️ 测试代码，不是交付物。
 *
 *  验证：parse → collector_start（内部走真 connect）→ 分频 tick（内部走真 read）
 *        真代码一行没改，只是把 IgH 用户态库换成了 ethercat_south_ecrt_stub.c。
 *
 *  必须在 test-file/ 目录下运行（parse 读的是相对路径 "JSON"）。
 * ============================================================================ */

#include <stdio.h>
#include <string.h>

#include "ethercat_south_collector.h"

/* 替身暴露的旋钮与计数器 */
extern ec_wc_state_t g_stub_wc_state;
extern int g_stub_receive_calls, g_stub_process_calls;
extern int g_stub_queue_calls, g_stub_send_calls;
extern int g_stub_release_calls;
extern int g_stub_sdo_calls, g_stub_dc_calls, g_stub_select_ref_calls;
extern int g_stub_app_time_calls, g_stub_sync_ref_calls, g_stub_sync_slave_calls;
extern uint16_t g_stub_last_sdo_index;
extern uint8_t  g_stub_last_sdo_subindex;
extern uint8_t  g_stub_last_sdo_data[32];
extern size_t   g_stub_last_sdo_len;
extern uint16_t g_stub_last_dc_assign;
extern uint32_t g_stub_last_dc_sync0_cycle;
extern int g_stub_fail_entry, g_stub_fail_sdo, g_stub_fail_dc;
extern void stub_reset(void);

static int fails = 0;

static void check(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? "OK" : "**FAIL**", what);
    if (!ok) fails++;
}

static Config cfg;
static Config cfg2;
static ethercat_south_collector_t col;
static ethercat_south_collector_t col2;
static ethercat_south_collector_t col3;

static void build_freerun_config(void)
{
    SlaveConfig *s;
    TagConfig *t;

    memset(&cfg2, 0, sizeof(cfg2));
    cfg2.slave_count = 1;

    s = &cfg2.slaves[0];
    strcpy(s->prefix, "test");
    strcpy(s->protocol, "ethercat");
    strcpy(s->node, "manual");
    s->alias = 0;
    s->position = 1;
    s->vendor_id = 0x00004321;
    s->product_code = 0x000000e2;
    s->mode = SOUTH_MODE_FREERUN;
    s->period_ms = 10;

    s->tag_count = 1;
    t = &s->tags[0];
    strcpy(t->name, "manual_in");
    strcpy(t->address, "0x6000:00");
    t->index = 0x6000;
    t->subindex = 0;
    t->raw_type = SOUTH_TYPE_I32;
    t->value_type = SOUTH_TYPE_I32;
    t->direction = SOUTH_DIR_IN;
    t->json_offset = 0;
    t->scale = 1.0;
    strcpy(t->unit, "none");
}

int main(void)
{
    const SlaveConfig *s;
    uint64_t t0;
    int t, i;

    /* ---- 真 parse ---- */
    printf("=== 0. parse ===\n");
    check("配置加载成功", configuration_store_load_startup(&cfg) == 0);
    check("只有 1 个 ethercat 从站", cfg.slave_count == 1);

    s = &cfg.slaves[0];
    check("node=motor_1", strcmp(s->node, "motor_1") == 0);
    check("alias=0 position=0", s->alias == 0 && s->position == 0);
    check("vendor/product", s->vendor_id == 0x00004321u &&
                             s->product_code == 0x000000e2u);
    check("mode=dc period=1", s->mode == SOUTH_MODE_DC && s->period_ms == 1);
    check("dc 参数", s->dc.assign_activate == 0x0300u &&
                     s->dc.sync0_cycle_ns == 1000000u);
    check("1 条启动 SDO", s->sdo_count == 1);
    check("SDO 内容", s->sdo[0].index == 0x6060 && s->sdo[0].subindex == 0 &&
                      s->sdo[0].data_type == SOUTH_TYPE_I8 &&
                      s->sdo[0].data_len == 1 && s->sdo[0].data[0] == 0x08 &&
                      s->sdo[0].apply == 1);
    check("6 个 tag", s->tag_count == 6);
    check("tag 顺序/方向",
          s->tags[0].index == 0x6064 && s->tags[0].direction == SOUTH_DIR_IN &&
          s->tags[1].index == 0x6041 && s->tags[1].direction == SOUTH_DIR_IN &&
          s->tags[2].index == 0x6040 && s->tags[2].direction == SOUTH_DIR_OUT &&
          s->tags[3].index == 0x607a && s->tags[3].direction == SOUTH_DIR_OUT &&
          s->tags[4].index == 0x6061 && s->tags[4].direction == SOUTH_DIR_IN &&
          s->tags[5].index == 0x603f && s->tags[5].direction == SOUTH_DIR_IN);

    /* ---- 1. 启动：SDO / DC / tag 注册 ---- */
    printf("\n=== 1. collector_start ===\n");
    stub_reset();
    check("start 返回 0", ethercat_south_collector_start(&col, &cfg) == 0);
    check("dc_enabled", col.dc_enabled == 1);
    check("过程映像非空，stub 域 24 字节",
          col.conn.domain_pd != NULL && col.conn.domain_size == 24);
    check("6 个 tag 都有偏移",
          col.conn.tag_offset[0][0] == 0 &&
          col.conn.tag_offset[0][1] == 4 &&
          col.conn.tag_offset[0][2] == 8 &&
          col.conn.tag_offset[0][3] == 12 &&
          col.conn.tag_offset[0][4] == 16 &&
          col.conn.tag_offset[0][5] == 20);

    check("SDO 调用 1 次", g_stub_sdo_calls == 1);
    check("SDO 参数", g_stub_last_sdo_index == 0x6060 &&
                      g_stub_last_sdo_subindex == 0 &&
                      g_stub_last_sdo_len == 1 &&
                      g_stub_last_sdo_data[0] == 0x08);
    check("DC 调用 1 次", g_stub_dc_calls == 1);
    check("DC 参数", g_stub_last_dc_assign == 0x0300 &&
                     g_stub_last_dc_sync0_cycle == 1000000);
    check("参考时钟选择 1 次", g_stub_select_ref_calls == 1);

    {
        int all_zero = 1;
        for (i = 0; i < (int)col.conn.domain_size; i++)
            if (col.conn.domain_pd[i] != 0) all_zero = 0;
        check("activate 后输出映像被清零", all_zero);
    }

    /* ---- 2. DC tick：application time / sync / 读取 ---- */
    printf("\n=== 2. DC tick ===\n");
    t0 = col.base_ms;
    for (t = 1; t <= 10; t++)
        check("tick 返回 0", ethercat_south_collector_tick(&col, t0 + (uint64_t)t) == 0);

    check("每拍 receive/process/queue/send 各 10 次",
          g_stub_receive_calls == 10 && g_stub_process_calls == 10 &&
          g_stub_queue_calls == 10 && g_stub_send_calls == 10);
    check("每拍 application_time / sync_ref / sync_slave 各 10 次",
          g_stub_app_time_calls == 10 && g_stub_sync_ref_calls == 10 &&
          g_stub_sync_slave_calls == 10);
    check("1 ms 周期读了 10 帧", col.read_count[0] == 10);

    check("帧 node_id=0 tag_count=6",
          col.frame.node_id == 0 && col.frame.tag_count == 6);
    check("in tag 读到 stub 图案",
          col.frame.tags[1].value.u16 == 0xA5A5 &&
          col.frame.tags[5].value.u16 == 0xA5A5);
    check("i32→f64 转换",
          col.frame.tags[0].value.f64 == (double)(int32_t)0xA5A5A5A5u);
    check("i8 转换", col.frame.tags[4].value.i8 == (int8_t)0xA5);
    check("out tag 固定为 0",
          col.frame.tags[2].value.u16 == 0 &&
          col.frame.tags[3].value.f64 == 0.0);
    check("帧 raw_type 是枚举",
          col.frame.tags[0].raw_type == SOUTH_TYPE_I32 &&
          col.frame.tags[1].raw_type == SOUTH_TYPE_U16);
    check("帧 address 与配置一致",
          strcmp(col.frame.tags[0].address, s->tags[0].address) == 0);

    /* ---- 3. 工作计数器 ---- */
    printf("\n=== 3. 工作计数器 ===\n");
    check("正常时不计数", col.wc_bad_count == 0);

    g_stub_wc_state = EC_WC_INCOMPLETE;
    ethercat_south_collector_tick(&col, t0 + 11);
    check("异常一拍 +1", col.wc_bad_count == 1);
    ethercat_south_collector_tick(&col, t0 + 12);
    check("连续异常 +2", col.wc_bad_count == 2);

    g_stub_wc_state = EC_WC_COMPLETE;
    ethercat_south_collector_tick(&col, t0 + 13);
    check("恢复后不再增加", col.wc_bad_count == 2);

    /* ---- 4. 非 DC 分支 ---- */
    printf("\n=== 4. freerun 分支 ===\n");
    build_freerun_config();
    stub_reset();
    check("start 返回 0", ethercat_south_collector_start(&col2, &cfg2) == 0);
    check("dc_enabled=0", col2.dc_enabled == 0);
    check("域 4 字节", col2.conn.domain_size == 4);

    t0 = col2.base_ms;
    for (t = 1; t <= 9; t++)
        ethercat_south_collector_tick(&col2, t0 + (uint64_t)t);
    check("10 ms 周期前 9 拍不读", col2.read_count[0] == 0);
    ethercat_south_collector_tick(&col2, t0 + 10);
    check("第 10 拍读 1 次", col2.read_count[0] == 1);
    check("非 DC 不调用 application_time/sync",
          g_stub_app_time_calls == 0 && g_stub_sync_ref_calls == 0 &&
          g_stub_sync_slave_calls == 0);

    /* ---- 5. 停机 ---- */
    printf("\n=== 5. stop ===\n");
    ethercat_south_collector_stop(&col);
    check("master 置 NULL", col.conn.master == NULL);
    check("release 调用过", g_stub_release_calls >= 1);
    ethercat_south_collector_stop(&col);
    check("stop 幂等", col.conn.master == NULL);

    ethercat_south_collector_stop(&col2);

    /* ---- 6. 注册失败 / SDO 失败 ---- */
    printf("\n=== 6. 启动失败路径 ===\n");
    stub_reset();
    g_stub_fail_entry = 0x6040;
    check("条目注册失败 → start -1",
          ethercat_south_collector_start(&col3, &cfg) != 0);
    check("失败后 master=NULL 且已 release",
          col3.conn.master == NULL && g_stub_release_calls >= 1);

    stub_reset();
    g_stub_fail_sdo = 1;
    check("SDO 入队失败 → start -1",
          ethercat_south_collector_start(&col3, &cfg) != 0);
    check("失败后 master=NULL 且已 release",
          col3.conn.master == NULL && g_stub_release_calls >= 1);

    printf("\n================ 失败 %d 项 ================\n", fails);
    return fails ? 1 : 0;
}
