/* ============================================================================
 *  ethercat_south_ecrt_stub.c —— 假的 IgH 用户态库（链接期替身）
 *
 *  ⚠️ 这不是交付物，也永远不该进入任何真实构建。
 *
 *  用途：在没有 EtherCAT 硬件、没有 libethercat、没有 root 的机器上，
 *        把 parse → connect → collector → read 整条链**用真代码**跑一遍。
 *        真代码一行不改，只是把 IgH 用户态库换成了这个替身。
 *
 *  为什么不做成"改 connect 让它支持假从站"：那样改的是交付物本身，
 *  而且会拆掉"从站不在线就起不来"这个响亮的失败信号 —— 主站会"成功"启动，
 *  read 却照读一块全 0 的过程映像，下游看到的是一条完全健康的流水线。
 *  替身只活在测试里，不存在这个问题。
 *
 *  假数据刻意摆成一眼就是假的图案（0xA5 / 0x5A 重复），不用 0、也不用
 *  任何"像真实读数"的值 —— 免得有人把测试输出当成真实数据。
 *
 *  与真 IgH 的差异（本替身够用就好，不追求保真）：
 *    · 偏移按"每条 4 字节、按注册顺序排"给，不按真实位长累加
 *    · 不模拟从站在线/离线（那是 WC 的活，测试里用 g_stub_wc_state 手动切）
 * ============================================================================ */

#warning "ethercat_south_ecrt_stub.c 是 IgH 替身桩，只用于无硬件验证，绝不允许进入任何真实构建！"

#include <string.h>
#include "ecrt.h"

/* ---------- 测试可以调的旋钮 ---------- */

/* 改成 EC_WC_INCOMPLETE / EC_WC_ZERO 就能模拟"从站没起来 / 掉线" */
ec_wc_state_t g_stub_wc_state = EC_WC_COMPLETE;
unsigned int  g_stub_wc       = 1;

/* 调用计数，方便断言"每拍都 receive/process/queue/send 了" */
int g_stub_receive_calls = 0;
int g_stub_process_calls = 0;
int g_stub_queue_calls   = 0;
int g_stub_send_calls    = 0;

/* ---------- 假的域与过程映像 ---------- */

#define STUB_MAX_DOMAINS   16
#define STUB_DOMAIN_BYTES  64

static uint8_t g_pd[STUB_MAX_DOMAINS][STUB_DOMAIN_BYTES];
static int     g_domain_n = 0;
static int     g_domain_off[STUB_MAX_DOMAINS];

/* 假图案：域 0 全 0xA5，域 1 全 0x5A，域 2 全 0xA5…… */
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

/* ---------- 被顶掉的 IgH 用户态 API ---------- */

void ecrt_release_master(ec_master_t *master)
{
    (void)master;                   /* 假：什么也不做 */
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
    return (ec_slave_config_t *)(void *)&dummy;         /* 非 NULL 即可 */
}

int ecrt_slave_config_reg_pdo_entry(ec_slave_config_t *sc, uint16_t entry_index,
                                    uint8_t entry_subindex, ec_domain_t *domain,
                                    unsigned int *bit_position)
{
    int k = domain_index_of(domain);
    (void)sc; (void)entry_index; (void)entry_subindex; (void)bit_position;

    if (k < 0) return -1;

    /* 假装每条占 4 字节、按注册顺序往后排 */
    if (g_domain_off[k] + 4 > STUB_DOMAIN_BYTES) return -1;
    g_domain_off[k] += 4;
    return g_domain_off[k] - 4;
}

int ecrt_slave_config_sdo8(ec_slave_config_t *sc, uint16_t sdo_index,
                           uint8_t sdo_subindex, uint8_t value)
{
    (void)sc; (void)sdo_index; (void)sdo_subindex; (void)value;
    return 0;                       /* 假装下下去了；真值只有伺服自己知道 */
}

int ecrt_master_activate(ec_master_t *master)
{
    int k;
    (void)master;

    /* 模拟"主站把每个域的过程映像填好了" —— 填成一眼是假的图案 */
    for (k = 0; k < g_domain_n; k++)
        memset(g_pd[k], stub_pattern(k), sizeof(g_pd[k]));

    return 0;
}

uint8_t *ecrt_domain_data(const ec_domain_t *domain)
{
    int k = domain_index_of(domain);
    return (k < 0) ? NULL : &g_pd[k][0];
}

/* 假：把"已注册的累计偏移"当作域大小。
   ⚠️ 真 IgH 里这个值在 activate 之前是未文档化行为，这里只是让测试能跑。 */
size_t ecrt_domain_size(const ec_domain_t *domain)
{
    int k = domain_index_of(domain);
    return (k < 0) ? (size_t)-1 : (size_t)g_domain_off[k];
}

int ecrt_master_receive(ec_master_t *master)   { (void)master; g_stub_receive_calls++; return 0; }
int ecrt_domain_process(ec_domain_t *domain)   { (void)domain; g_stub_process_calls++; return 0; }
int ecrt_domain_queue(ec_domain_t *domain)     { (void)domain; g_stub_queue_calls++;   return 0; }
int ecrt_master_send(ec_master_t *master)      { (void)master; g_stub_send_calls++;    return 0; }

int ecrt_domain_state(const ec_domain_t *domain, ec_domain_state_t *state)
{
    (void)domain;
    if (state == NULL) return -1;

    state->working_counter   = g_stub_wc;
    state->wc_state          = g_stub_wc_state;
    state->redundancy_active = 0;
    return 0;
}
