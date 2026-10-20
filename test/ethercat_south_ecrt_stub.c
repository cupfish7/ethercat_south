/* ============================================================================
 *  ethercat_south_ecrt_stub.c —— 假的 IgH 用户态库（链接期替身）
 *
 *  ⚠️ 这不是交付物，也永远不该进入任何真实构建。
 *
 *  用途：在没有 EtherCAT 硬件、没有 libethercat、没有 root 的机器上，
 *        把 parse → connect → collector → read 整条链**用真代码**跑一遍。
 *        真代码一行不改，只是把 IgH 用户态库换成了这个替身。
 *
 *  假数据刻意摆成一眼就是假的图案（0xA5 / 0x5A 重复）。
 *
 *  与真 IgH 的差异：
 *    · 偏移按"每条 4 字节、按注册顺序排"给，不按真实位长累加
 *    · 不模拟从站在线/离线（那是 WC 的活，测试里用 g_stub_wc_state 手动切）
 * ============================================================================ */

#warning "ethercat_south_ecrt_stub.c 是 IgH 替身桩，只用于无硬件验证，绝不允许进入任何真实构建！"

#include <string.h>
#include "ecrt.h"

/* ---------- 测试可以调的旋钮 ---------- */

ec_wc_state_t g_stub_wc_state = EC_WC_COMPLETE;
unsigned int  g_stub_wc       = 1;

/* 调用计数 */
int g_stub_receive_calls = 0;
int g_stub_process_calls = 0;
int g_stub_queue_calls   = 0;
int g_stub_send_calls    = 0;
int g_stub_release_calls = 0;

int g_stub_sdo_calls         = 0;
int g_stub_dc_calls          = 0;
int g_stub_select_ref_calls  = 0;
int g_stub_app_time_calls    = 0;
int g_stub_sync_ref_calls    = 0;
int g_stub_sync_slave_calls  = 0;

/* 最后一次调用的参数 */
uint16_t g_stub_last_sdo_index = 0;
uint8_t  g_stub_last_sdo_subindex = 0;
uint8_t  g_stub_last_sdo_data[32];
size_t   g_stub_last_sdo_len = 0;

uint16_t g_stub_last_dc_assign = 0;
uint32_t g_stub_last_dc_sync0_cycle = 0;
int32_t  g_stub_last_dc_sync0_shift = 0;
uint32_t g_stub_last_dc_sync1_cycle = 0;
int32_t  g_stub_last_dc_sync1_shift = 0;
uint64_t g_stub_last_app_time = 0;

/* 失败注入 */
int g_stub_fail_entry = -1;   /* 匹配这个 entry_index 时返回 -1 */
int g_stub_fail_sdo   = 0;
int g_stub_fail_dc    = 0;

/* ---------- 假的域与过程映像 ---------- */

#define STUB_MAX_DOMAINS   16
#define STUB_DOMAIN_BYTES 256

static uint8_t g_pd[STUB_MAX_DOMAINS][STUB_DOMAIN_BYTES];
static int     g_domain_n = 0;
static int     g_domain_off[STUB_MAX_DOMAINS];

static uint8_t stub_pattern(int k)
{
    return (uint8_t)((k & 1) ? 0x5A : 0xA5);
}

static int domain_index_of(const ec_domain_t *d)
{
    int k;
    for (k = 0; k < g_domain_n; k++)
        if ((const void *)&g_pd[k][0] == (const void *)d) return k;
    return -1;
}

void stub_reset(void)
{
    int k;

    g_domain_n = 0;
    memset(g_domain_off, 0, sizeof(g_domain_off));
    memset(g_pd, 0, sizeof(g_pd));

    g_stub_wc_state = EC_WC_COMPLETE;
    g_stub_wc = 1;

    g_stub_receive_calls = 0;
    g_stub_process_calls = 0;
    g_stub_queue_calls   = 0;
    g_stub_send_calls    = 0;
    g_stub_release_calls = 0;

    g_stub_sdo_calls        = 0;
    g_stub_dc_calls         = 0;
    g_stub_select_ref_calls = 0;
    g_stub_app_time_calls   = 0;
    g_stub_sync_ref_calls   = 0;
    g_stub_sync_slave_calls = 0;

    g_stub_last_sdo_index = 0;
    g_stub_last_sdo_subindex = 0;
    g_stub_last_sdo_len = 0;
    memset(g_stub_last_sdo_data, 0, sizeof(g_stub_last_sdo_data));

    g_stub_last_dc_assign = 0;
    g_stub_last_dc_sync0_cycle = 0;
    g_stub_last_dc_sync0_shift = 0;
    g_stub_last_dc_sync1_cycle = 0;
    g_stub_last_dc_sync1_shift = 0;
    g_stub_last_app_time = 0;

    g_stub_fail_entry = -1;
    g_stub_fail_sdo   = 0;
    g_stub_fail_dc    = 0;

    for (k = 0; k < STUB_MAX_DOMAINS; k++)
        memset(g_pd[k], 0, sizeof(g_pd[k]));
}

/* ---------- 被顶掉的 IgH 用户态 API ---------- */

void ecrt_release_master(ec_master_t *master)
{
    (void)master;
    g_stub_release_calls++;
}

ec_master_t *ecrt_request_master(unsigned int master_index)
{
    (void)master_index;
    return (ec_master_t *)(void *)&g_pd[0][0];          /* 非 NULL 即可 */
}

ec_domain_t *ecrt_master_create_domain(ec_master_t *master)
{
    (void)master;
    if (g_domain_n >= STUB_MAX_DOMAINS) return NULL;

    g_domain_off[g_domain_n] = 0;
    return (ec_domain_t *)(void *)&g_pd[g_domain_n++][0];
}

ec_slave_config_t *ecrt_master_slave_config(ec_master_t *master, uint16_t alias,
                                            uint16_t position, uint32_t vendor_id,
                                            uint32_t product_code)
{
    static int dummy;
    (void)master; (void)alias; (void)position; (void)vendor_id; (void)product_code;
    return (ec_slave_config_t *)(void *)&dummy;
}

int ecrt_slave_config_reg_pdo_entry(ec_slave_config_t *sc, uint16_t entry_index,
                                    uint8_t entry_subindex, ec_domain_t *domain,
                                    unsigned int *bit_position)
{
    int k = domain_index_of(domain);
    (void)sc; (void)entry_subindex; (void)bit_position;

    if (g_stub_fail_entry >= 0 && entry_index == (uint16_t)g_stub_fail_entry)
        return -1;
    if (k < 0) return -1;

    /* 假装每条占 4 字节、按注册顺序往后排 */
    if (g_domain_off[k] + 4 > STUB_DOMAIN_BYTES) return -1;
    g_domain_off[k] += 4;
    return g_domain_off[k] - 4;
}

int ecrt_slave_config_sdo(ec_slave_config_t *sc, uint16_t index,
                          uint8_t subindex, const uint8_t *data, size_t size)
{
    size_t n;

    (void)sc;

    if (g_stub_fail_sdo) return -1;

    g_stub_sdo_calls++;
    g_stub_last_sdo_index = index;
    g_stub_last_sdo_subindex = subindex;
    g_stub_last_sdo_len = size;

    n = size < sizeof(g_stub_last_sdo_data) ?
        size : sizeof(g_stub_last_sdo_data);
    if (data != NULL && n > 0) memcpy(g_stub_last_sdo_data, data, n);

    return 0;
}

int ecrt_slave_config_sdo8(ec_slave_config_t *sc, uint16_t index,
                           uint8_t subindex, uint8_t value)
{
    uint8_t data = value;
    return ecrt_slave_config_sdo(sc, index, subindex, &data, 1);
}

int ecrt_slave_config_dc(ec_slave_config_t *sc, uint16_t assign_activate,
                         uint32_t sync0_cycle, int32_t sync0_shift,
                         uint32_t sync1_cycle, int32_t sync1_shift)
{
    (void)sc;

    if (g_stub_fail_dc) return -1;

    g_stub_dc_calls++;
    g_stub_last_dc_assign = assign_activate;
    g_stub_last_dc_sync0_cycle = sync0_cycle;
    g_stub_last_dc_sync0_shift = sync0_shift;
    g_stub_last_dc_sync1_cycle = sync1_cycle;
    g_stub_last_dc_sync1_shift = sync1_shift;
    return 0;
}

int ecrt_master_select_reference_clock(ec_master_t *master, ec_slave_config_t *sc)
{
    (void)master; (void)sc;
    g_stub_select_ref_calls++;
    return 0;
}

int ecrt_master_application_time(ec_master_t *master, uint64_t app_time)
{
    (void)master;
    g_stub_app_time_calls++;
    g_stub_last_app_time = app_time;
    return 0;
}

int ecrt_master_sync_reference_clock(ec_master_t *master)
{
    (void)master;
    g_stub_sync_ref_calls++;
    return 0;
}

int ecrt_master_sync_slave_clocks(ec_master_t *master)
{
    (void)master;
    g_stub_sync_slave_calls++;
    return 0;
}

int ecrt_master_activate(ec_master_t *master)
{
    int k;
    (void)master;

    /* 模拟主站把每个域的过程映像填好了 */
    for (k = 0; k < g_domain_n; k++)
        memset(g_pd[k], stub_pattern(k), sizeof(g_pd[k]));

    return 0;
}

uint8_t *ecrt_domain_data(const ec_domain_t *domain)
{
    int k = domain_index_of(domain);
    return (k < 0) ? NULL : &g_pd[k][0];
}

size_t ecrt_domain_size(const ec_domain_t *domain)
{
    int k = domain_index_of(domain);
    return (k < 0) ? (size_t)-1 : (size_t)g_domain_off[k];
}

int ecrt_master_receive(ec_master_t *master)
{
    int k;
    (void)master;

    /* connect 在 activate 之后会把过程映像清零；
       每次 receive 重新铺一遍假图案，保证 read 能读到确定的数据。 */
    for (k = 0; k < g_domain_n; k++)
        memset(g_pd[k], stub_pattern(k), sizeof(g_pd[k]));

    g_stub_receive_calls++;
    return 0;
}

int ecrt_domain_process(ec_domain_t *domain)
{
    (void)domain;
    g_stub_process_calls++;
    return 0;
}

int ecrt_domain_queue(ec_domain_t *domain)
{
    (void)domain;
    g_stub_queue_calls++;
    return 0;
}

int ecrt_master_send(ec_master_t *master)
{
    (void)master;
    g_stub_send_calls++;
    return 0;
}

int ecrt_domain_state(const ec_domain_t *domain, ec_domain_state_t *state)
{
    (void)domain;
    if (state == NULL) return -1;

    state->working_counter   = g_stub_wc;
    state->wc_state          = g_stub_wc_state;
    state->redundancy_active = 0;
    return 0;
}
