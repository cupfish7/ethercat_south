/* ============================================================================
 *  ethercat_south_connect.c —— 南向 EtherCAT 主站接入
 *
 *  职责：把 JSON 解析出来的 Config，翻译成 IgH 主站的配置调用。
 *
 *  本文件依赖 ecrt.h 和 libethercat，二者都是 Linux 专有，
 *  不能在 Windows 上编译。构建方法见文件末尾。
 *
 *  步骤实现图
 *  ----------
 *
 *    [JSON 目录下的 .json 文件]
 *          │  configuration_store_load_startup()      ← ethercat_south_parse.c
 *          ▼
 *    [Config { SlaveConfig[] }]
 *          │  只保留 protocol == "ethercat" 的从站
 *          ▼
 *    ①  ecrt_request_master(0)
 *          ▼
 *    ②  ecrt_master_create_domain()         建**唯一**的域
 *          ▼
 *    ③  对每个从站：
 *    │     ecrt_master_slave_config(alias, position, vendor_id, product_code)
 *    │     ├─ 逐条 start_sdo：ecrt_slave_config_sdo()
 *    │     ├─ mode==dc：ecrt_slave_config_dc()
 *    │     └─ 逐 tag：ecrt_slave_config_reg_pdo_entry()
 *    │            → 返回该条目在过程映像里的字节偏移
 *          ▼
 *    ④  ecrt_master_activate()
 *          ▼
 *    ⑤  ecrt_domain_data() + ecrt_domain_size()
 *    │     └ **memset(domain_pd, 0)** —— 输出映像必须从确定的 0 开始
 *          ▼
 *    [conn->domain_pd / tag_offset]          read 模块只读这些字段
 *
 *  输入与输出的对应关系
 *  --------------------
 *    SlaveConfig.alias / position           →  ecrt_master_slave_config 的寻址
 *    SlaveConfig.vendor_id / product_code   →  期望身份
 *    SlaveConfig.sdo[]                      →  ecrt_slave_config_sdo
 *    SlaveConfig.mode / dc                  →  ecrt_slave_config_dc
 *    TagConfig.index / subindex             →  PDO 条目寻址
 *    TagConfig.json_offset                  →  【仅参考】真实偏移由 IgH 分配
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ethercat_south_connect.h"
#include "ecrt.h"

int ethercat_south_master_start(const Config *config, ethercat_south_conn_t *conn)
{
    int i, j;
    int any_dc = 0;

    if (config == NULL || conn == NULL) return -1;
    if (config->slave_count <= 0) {
        printf("配置里没有从站\n");
        return -1;
    }

    memset(conn, 0, sizeof(*conn));
    conn->config = config;

    /* tag_offset 的 0 是合法偏移，所以未注册必须用 -1 表示。 */
    for (i = 0; i < MAX_SLAVES; i++)
        for (j = 0; j < MAX_TAGS; j++)
            conn->tag_offset[i][j] = -1;

    /* ① 请求主站 —— 必须是第一个调用，向内核模块独占申请 0 号主站。
          NULL 通常意味着 ethercat 内核模块没加载，或者主站被别的进程占着。 */
    conn->master = ecrt_request_master(0);
    if (conn->master == NULL) {
        printf("ecrt_request_master 失败\n");
        return -1;
    }

    /* ② 建**唯一**的域。域太大时主站会自动拆成多个 datagram。 */
    conn->domain = ecrt_master_create_domain(conn->master);
    if (conn->domain == NULL) {
        printf("建域失败\n");
        goto fail;
    }

    /* ③ 逐个从站：应用启动 SDO / DC，注册 PDO 条目 */
    for (i = 0; i < config->slave_count; i++) {
        const SlaveConfig *s = &config->slaves[i];
        ec_slave_config_t *sc;

        if (strcmp(s->protocol, "ethercat") != 0) continue;

        sc = ecrt_master_slave_config(conn->master, s->alias, s->position,
                                      s->vendor_id, s->product_code);
        if (sc == NULL) {
            printf("[%s] ecrt_master_slave_config 失败（alias=%u position=%u）\n",
                   s->node, s->alias, s->position);
            goto fail;
        }

        /* 启动 SDO：必须在 activate 之前入队。
           IgH 会在从站到达 PREOP 后统一下发；这里不做 transition 区分。 */
        for (j = 0; j < s->sdo_count; j++) {
            const StartSdoConfig *sd = &s->sdo[j];

            if (!sd->apply) continue;

            if (ecrt_slave_config_sdo(sc, sd->index, sd->subindex,
                                      sd->data, sd->data_len) != 0) {
                printf("[%s] 启动 SDO 0x%04X:%02X 入队失败\n",
                       s->node, sd->index, sd->subindex);
                goto fail;
            }
        }

        /* DC：从站级配置，必须在 activate 之前。 */
        if (s->mode == SOUTH_MODE_DC) {
            if (ecrt_slave_config_dc(sc,
                                     s->dc.assign_activate,
                                     s->dc.sync0_cycle_ns,
                                     s->dc.sync0_shift_ns,
                                     s->dc.sync1_cycle_ns,
                                     s->dc.sync1_shift_ns) != 0) {
                printf("[%s] ecrt_slave_config_dc 失败\n", s->node);
                goto fail;
            }
            any_dc = 1;
        }

        /* 逐 tag 注册。注册一个条目会把该条目所在的**整个同步管理器**
           追加进 domain；同一 SM 的后续条目复用 FMMU，只返回偏移。
           direction=="out" 的条目也要注册，否则对应 SM 不在周期数据里。 */
        for (j = 0; j < s->tag_count; j++) {
            const TagConfig *t = &s->tags[j];
            int off;

            off = ecrt_slave_config_reg_pdo_entry(sc, t->index, t->subindex,
                                                  conn->domain, NULL);
            if (off < 0) {
                printf("[%s.%s] 注册 PDO 条目 0x%04X:%02X 失败"
                       "（条目不在 SII 默认映射，或从站不在线）\n",
                       s->node, t->name, t->index, t->subindex);
                goto fail;
            }

            conn->tag_offset[i][j] = off;
        }
    }

    /* DC 参考时钟：默认让 IgH 选第一个 DC-capable 从站。 */
    if (any_dc && ecrt_master_select_reference_clock(conn->master, NULL) != 0) {
        printf("ecrt_master_select_reference_clock 失败\n");
        goto fail;
    }

    /* ④ 激活主站 —— 配置阶段结束。 */
    if (ecrt_master_activate(conn->master) != 0) {
        printf("ecrt_master_activate 失败\n");
        goto fail;
    }

    /* ⑤ 取过程映像 —— 用户态必须在 activate 之后才能调。 */
    conn->domain_pd = ecrt_domain_data(conn->domain);
    if (conn->domain_pd == NULL) {
        printf("取过程映像失败\n");
        goto fail;
    }

    conn->domain_size = ecrt_domain_size(conn->domain);
    if (conn->domain_size == 0 || conn->domain_size == (size_t)-1 ||
        conn->domain_size > 0x80000000UL) {
        printf("域大小异常：%lu 字节\n", (unsigned long)conn->domain_size);
        goto fail;
    }

    /* 关键安全步骤：IgH 的域内存来自 kmalloc，不保证为 0。
       当前不写任何 RxPDO 输出，必须先把整个过程映像清零，
       否则会向从站发送内核堆里的随机控制字/目标位置。 */
    memset(conn->domain_pd, 0, conn->domain_size);

    return 0;

fail:
    if (conn->master != NULL) {
        ecrt_release_master(conn->master);
        conn->master = NULL;
    }
    conn->domain = NULL;
    conn->domain_pd = NULL;
    conn->domain_size = 0;
    return -1;
}

/* ---------- 测试 / 演示 ---------- */

void ethercat_south_connect_test(void)
{
    static Config config;                   /* 约 1.5 MB，必须离开栈 */
    static ethercat_south_conn_t conn;
    int i;

    if (configuration_store_load_startup(&config) != 0) {
        printf("配置加载失败\n");
        return;
    }

    if (ethercat_south_master_start(&config, &conn) != 0) {
        printf("主站启动失败\n");
        return;
    }

    printf("域: %lu 字节（单域）@ %p\n",
           (unsigned long)conn.domain_size, (void *)conn.domain_pd);

    for (i = 0; i < config.slave_count; i++) {
        const SlaveConfig *s = &config.slaves[i];
        int j;

        if (strcmp(s->protocol, "ethercat") != 0) continue;

        printf("从站 %d: %s  alias=%u position=%u  vendor 0x%08X product 0x%08X"
               "  mode=%s  period=%d ms  sdo=%d\n",
               i, s->node, s->alias, s->position, s->vendor_id,
               s->product_code, south_mode_name(s->mode), s->period_ms,
               s->sdo_count);

        for (j = 0; j < s->tag_count; j++) {
            const TagConfig *t = &s->tags[j];

            printf("  [%d] %-16s %-12s -> 0x%04X:%02X  %2d bit"
                   "  dir=%s  json_offset=%d  IgH offset=%d\n",
                   j, t->name, t->address, t->index, t->subindex,
                   south_type_bit_length(t->raw_type),
                   t->direction == SOUTH_DIR_OUT ? "out" : "in",
                   t->json_offset, conn.tag_offset[i][j]);
        }
    }
}

/* ============================================================================
 *  构建（只能在 Linux 上做，Windows 没有 ecrt.h / libethercat）：
 *
 *    gcc -Wall -g -I<test-file> -I<ethercat-1_6-stable>/include \
 *        ethercat_south_connect.c ethercat_south_parse.c cJSON/cJSON.c \
 *        -o connect_test -lethercat
 *
 *    sudo ./connect_test
 *      需要 root 权限 + 已加载 ethercat 内核模块 + 真实从站。
 * ============================================================================ */
