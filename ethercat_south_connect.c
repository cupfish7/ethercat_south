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
 *          │  只挑 protocol == "ethercat" 的从站
 *          ▼
 *    ①  ecrt_request_master(0)              向内核独占申请主站
 *          ▼
 *    ②  ecrt_master_create_domain()         建过程数据域（过程映像的容器）
 *          ▼
 *    ③  对每个 ethercat 从站：
 *    │     ecrt_master_slave_config(alias, position, vendor_id, product_code)
 *    │       └─ 内部已自动加载从站的默认同步管理器 / PDO 映射
 *    │     └ 对每个 tag：
 *    │          ecrt_slave_config_reg_pdo_entry(0x6064, 0x00, domain, NULL)
 *    │            → 返回该条目在过程映像里的【字节偏移】
 *          ▼
 *    ④  ecrt_master_activate()              ★ 配置阶段结束的标志
 *    │                                       之后 IgH 内核状态机接管，
 *    │                                       自动推进从站 INIT→PREOP→SAFEOP→OP
 *          ▼
 *    ⑤  ecrt_domain_data()                  取过程映像首地址
 *          ▼
 *    [conn->domain_pd[]]   各域的过程映像基址，读/写过程数据用
 *                          （本模块到此为止，不做周期任务）
 *
 *  输入与输出的对应关系
 *  --------------------
 *    SlaveConfig.slave         "0/0"        →  alias / position
 *    SlaveConfig.vendor_id                  →  ecrt_master_slave_config 的期望厂商 ID
 *    SlaveConfig.product_code               →  ecrt_master_slave_config 的期望产品码
 *    TagConfig.address         "0x6064:00"  →  PDO 条目 index / subindex
 *    TagConfig.raw_type        "i32"        →  位长度（本模块只打印，不喂给 IgH）
 *    TagConfig.direction       "in"         →  （走默认映射，本模块未使用）
 *    TagConfig.offset          0/4/8        →  【仅参考】真实偏移由 IgH 分配
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ethercat_south_connect.h"
#include "ecrt.h"          /* IgH 主站用户态 API，来自 ethercat-1.6-stable/include */

/* ---------- 模块级状态 ----------
   域、过程映像基址、从站→域映射、tag 偏移表，全部收在 ethercat_south_conn_t 里，
   由调用方（将来的 collector）持有 —— 本文件不再有隐藏全局，
   于是 read 模块的行为完全由传进来的 conn 决定，不依赖本文件的状态。
   结构体定义见 ethercat_south_connect.h。 */

/* ---------- 配置字符串 → IgH 参数 的三个解析工具 ---------- */

/* 拆 "0/0" → alias / position。
   alias 为 0 时 position 就是环上位置；alias 非 0 时 position 是相对该别名从站的偏移。 */
static int parse_slave_addr(const char *s, uint16_t *alias, uint16_t *position)
{
    char *end = NULL;
    unsigned long a, p;

    if (s == NULL || *s == '\0') return -1;

    a = strtoul(s, &end, 0);                 /* base 0：自动识别 0x 前缀 */
    if (end == s || *end != '/') return -1;

    s = end + 1;
    p = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return -1;
    if (a > 0xFFFFUL || p > 0xFFFFUL) return -1;

    *alias    = (uint16_t)a;
    *position = (uint16_t)p;
    return 0;
}

/* 拆 "0x6064:00" → PDO 条目 index / subindex。
   冒号左边是对象字典索引，右边是子索引。 */
static int parse_pdo_entry_addr(const char *s, uint16_t *index, uint8_t *subindex)
{
    char *end = NULL;
    unsigned long idx, sub;

    if (s == NULL || *s == '\0') return -1;

    idx = strtoul(s, &end, 0);
    if (end == s || *end != ':') return -1;

    s = end + 1;
    sub = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return -1;
    if (idx > 0xFFFFUL || sub > 0xFFUL) return -1;

    *index    = (uint16_t)idx;
    *subindex = (uint8_t)sub;
    return 0;
}

/* raw_type 字符串 → 位长度。
   IgH 描述一个 PDO 条目用的是"占多少位"，配置里用的是类型名，这里做换算。
   本模块只拿它打印 —— 走默认映射时不需要把它喂给 IgH。 */
static int raw_type_bit_length(const char *raw_type)
{
    if (raw_type == NULL) return -1;
    if (strcmp(raw_type, "bool") == 0 ||
        strcmp(raw_type, "i8")   == 0 || strcmp(raw_type, "u8")   == 0) return 8;
    if (strcmp(raw_type, "i16")  == 0 || strcmp(raw_type, "u16")  == 0) return 16;
    if (strcmp(raw_type, "i32")  == 0 || strcmp(raw_type, "u32")  == 0 ||
        strcmp(raw_type, "real") == 0) return 32;
    if (strcmp(raw_type, "i64")  == 0 || strcmp(raw_type, "u64")  == 0 ||
        strcmp(raw_type, "lreal") == 0) return 64;
    return -1;
}

/* ---------- 采样周期分组 ---------- */

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);          /* 不写成 x - y，避免溢出 */
}

/* 收集 config 里出现过的不同 period_ms，升序排序后写入 out，返回个数。
   超过 max 返回 -1；period_ms <= 0 的从站不参与分组（无法决定采样组）。
   排序是为了让域编号稳定：readdir 的顺序不确定，不排的话同一份配置两次运行
   可能得到不同的域编号，输出就没法比对了。 */
static int collect_periods(const Config *config, int *out, int max)
{
    int n = 0;
    int i, k;

    for (i = 0; i < config->slave_count; i++) {
        int p = config->slaves[i].period_ms;

        if (strcmp(config->slaves[i].protocol, "ethercat") != 0) continue;
        if (p <= 0) continue;

        for (k = 0; k < n; k++) {
            if (out[k] == p) break;
        }
        if (k < n) continue;                        /* 这个周期已经有了 */

        if (n >= max) return -1;
        out[n++] = p;
    }

    qsort(out, (size_t)n, sizeof(out[0]), cmp_int);
    return n;
}

/* 按采样周期查域下标，没有返回 -1。域在第②步一次性建好，这里只查不建。 */
static int domain_index_of(const ethercat_south_conn_t *conn, int period_ms)
{
    int k;
    for (k = 0; k < conn->domain_count; k++) {
        if (conn->domain_period[k] == period_ms) return k;
    }
    return -1;
}

/* ---------- 主流程 ---------- */

int ethercat_south_master_start(const Config *config, ethercat_south_conn_t *conn)
{
    int i;

    if (config == NULL || conn == NULL) return -1;

    /* 全部清零：域、过程映像、映射表、偏移表都从 0 开始。
       跳过的从站 slave_domain 会保持 0 —— 所以下面显式写成 -1，不能靠清零。 */
    memset(conn, 0, sizeof(*conn));
    conn->config = config;

    /* ① 请求主站 —— 必须是第一个调用，向内核模块独占申请 0 号主站。
          NULL 通常意味着 ethercat 内核模块没加载，或者主站被别的进程占着。 */
    conn->master = ecrt_request_master(0);
    if (conn->master == NULL) {
        printf("ecrt_request_master 失败\n");
        return -1;
    }

    /* ② 按采样周期建域 —— 有几个不同的 period_ms 就建几个域。
          域是一块连续的过程映像内存，activate 之后用 ecrt_domain_data() 拿各自的首地址。
          先扫一遍 config 把周期收集齐再统一建，这样域编号一次定死、后面只查不建。 */
    {
        int periods[MAX_DOMAINS];
        int k;

        conn->domain_count = collect_periods(config, periods, MAX_DOMAINS);
        if (conn->domain_count < 0) {
            printf("采样周期种类超过 MAX_DOMAINS=%d\n", MAX_DOMAINS);
            return -1;
        }

        for (k = 0; k < conn->domain_count; k++) {
            conn->domain_period[k] = periods[k];
            conn->domains[k] = ecrt_master_create_domain(conn->master);
            if (conn->domains[k] == NULL) {
                printf("建域失败（period %d ms）\n", periods[k]);
                return -1;
            }
        }
    }

    /* ③ 逐个从站：挑出 ethercat 的，定出它属于哪个域，再注册它的 PDO 条目 */
    for (i = 0; i < config->slave_count; i++) {
        const SlaveConfig *s = &config->slaves[i];
        ec_slave_config_t *sc;
        uint16_t alias, position;
        int j, k;

        conn->slave_domain[i] = -1;                 /* 先清，跳过的从站就保持 -1 */

        if (strcmp(s->protocol, "ethercat") != 0) continue;   /* 其它协议不归这里管 */

        /* 采样周期决定它进哪个域；周期无效（<=0，含字段缺失）就没法分组 */
        k = domain_index_of(conn, s->period_ms);
        if (k < 0) {
            printf("[%s] period_ms=%d 无效，跳过\n", s->node, s->period_ms);
            continue;
        }

        if (parse_slave_addr(s->slave, &alias, &position) != 0) {
            printf("[%s] slave 字段 \"%s\" 解析失败，跳过\n", s->node, s->slave);
            continue;
        }

        /* vendor_id / product_code 是「期望值」：主站扫描到该位置的从站后，
           会拿从站 SII 里的值来比对，对不上就报错并且不配置它。
           注意 0 不是通配符（通配符是 0xffffffff，而且需要主站以 --enable-wildcards
           编译过才生效），所以这两个字段必须填真实值。

           另外：此刻从站可以不在总线上，本调用照样返回有效指针 ——
           真正的绑定发生在之后的网络扫描阶段，不是现在。 */
        sc = ecrt_master_slave_config(conn->master, alias, position,
                                      s->vendor_id, s->product_code);
        if (sc == NULL) {
            printf("[%s] ecrt_master_slave_config 失败（alias=%u position=%u）\n",
                   s->node, alias, position);
            continue;
        }

        conn->slave_domain[i] = k;  /* 记下这个从站属于哪个域；偏移要靠它才有意义 */

        /* ---- L6N 伺服控制：配置期的两件事 ------------------------------------
           ① 把运行模式写成 CSP(8)（走 SDO —— 本从站默认 RxPDO 里没有 0x6060）
           ② 注册它自己用的写/读条目（控制字、目标位置、状态字、实际位置、模式显示）
           配置期只做这两件；状态机和运动指令是**周期期**的事，由 collector 每拍调
           ethercat_south_l6n_step()。放在这里是因为它们都必须在 activate 之前完成。

           ⚠️ 注册失败不中止整个启动 —— 该从站退化成"只采集不控制"。 */
        ethercat_south_l6n_setup(sc, &conn->l6n[i],
                                 ETHERCAT_SOUTH_L6N_AMP_COUNTS,
                                 ETHERCAT_SOUTH_L6N_FREQ_HZ);
        if (ethercat_south_l6n_reg_pdo(sc, conn->domains[k], &conn->l6n[i]) != 0)
            printf("[%s] L6N 控制条目注册失败，该从站只采集不控制\n", s->node);

        /* 这里刻意不调 ecrt_slave_config_pdos()：
           上面那句内部已经调过 ec_slave_config_load_default_sync_config()，
           从站默认的同步管理器和 PDO 映射已经就绪，直接按 index/subindex 注册条目即可。
           代价是：从站必须在总线上，否则下面的条目注册会失败。 */ 

        for (j = 0; j < s->tag_count; j++) {
            const TagConfig *t = &s->tags[j];
            uint16_t index;
            uint8_t subindex;
            int off;

            if (parse_pdo_entry_addr(t->address, &index, &subindex) != 0) {
                printf("[%s.%s] address \"%s\" 解析失败，跳过\n",
                       s->node, t->name, t->address);
                continue;
            }

            /* 注册进【该从站采样周期对应的那个域】，不是全局唯一的一个。
               返回值是这个条目在【那个域的过程映像】里的字节偏移 ——
               偏移由 IgH 在 activate 时统一分配，配置里指定不了，
               所以 JSON 里的 offset 字段只作参考，代码不用它。
               bit_position 传 NULL：条目若不是字节对齐（例如布尔量）会直接报错。 */
            off = ecrt_slave_config_reg_pdo_entry(sc, index, subindex, conn->domains[k], NULL);
            if (off < 0) {
                printf("[%s.%s] 注册 PDO 条目 0x%04X:%02X 失败（从站在总线上吗？）\n",
                       s->node, t->name, index, subindex);
                continue;
            }
            conn->tag_offset[i][j] = (unsigned int)off;
        }
    }

    /* ④ 激活主站 —— 配置阶段结束的标志。
          它分配域内存、计算 FMMU 逻辑地址，并通知内核状态机把配置下发到网络。
          此后状态机接管，自动推进从站 INIT→PREOP→SAFEOP→OP。

          返回 0 只代表「域收尾成功 + 操作线程已启动」，不代表从站已经进 OP。
          而且从这一步起，周期收发（ecrt_master_receive / ecrt_master_send）
          变成应用自己的责任了 —— 本模块到此为止，不做周期任务。 */
    if (ecrt_master_activate(conn->master) != 0) {
        printf("ecrt_master_activate 失败\n");
        return -1;
    }

    /* ⑤ 逐个域取过程映像首地址 —— 用户态必须在 activate 之后才能调。
          各域在同一块映射区里依次排布，所以每个域一个基址，不能共用一个。 */
    {
        int k;
        for (k = 0; k < conn->domain_count; k++) {
            conn->domain_pd[k] = ecrt_domain_data(conn->domains[k]);
            if (conn->domain_pd[k] == NULL) {
                printf("取过程映像失败（period %d ms）\n", conn->domain_period[k]);
                return -1;
            }
        }
    }

    return 0;
}

/* ---------- 测试 / 演示 ---------- */

void ethercat_south_connect_test(void)
{
    static Config config;                   /* 约 1.34 MB，必须离开栈 */
    static ethercat_south_conn_t conn;      /* 约 33 KB，同样必须离开栈 */
    int i, k;

    /* 第一步：加载 JSON 配置（这活归 parse 模块） */
    if (configuration_store_load_startup(&config) != 0) {
        printf("配置加载失败\n");
        return;
    }

    /* 第二步：按配置启动主站，结果写进 conn */
    if (ethercat_south_master_start(&config, &conn) != 0) {
        printf("主站启动失败\n");
        return;
    }

    /* 第三步：打印结果。
       先看域清单（域是按采样周期分的），再看每个从站落在哪个域。
       每个 tag 那行最右边两列 —— 配置里写的 offset（0/4/8）和 IgH 实际分配的
       偏移没有关系，真实偏移只有注册完才知道。 */
    printf("共 %d 个域（按采样周期分）：\n", conn.domain_count);
    for (k = 0; k < conn.domain_count; k++) {
        printf("  域 %d: period %3d ms, 过程映像 @ %p\n",
               k, conn.domain_period[k], (void *)conn.domain_pd[k]);
    }

    for (i = 0; i < config.slave_count; i++) {
        const SlaveConfig *s = &config.slaves[i];
        int j;

        if (strcmp(s->protocol, "ethercat") != 0) continue;

        printf("从站 %d: %s   vendor 0x%08X  product 0x%08X  slave \"%s\""
               "  → 域 %d (%d ms)\n",
               i, s->node, s->vendor_id, s->product_code, s->slave,
               conn.slave_domain[i],
               (conn.slave_domain[i] >= 0) ? conn.domain_period[conn.slave_domain[i]] : 0);

        for (j = 0; j < s->tag_count; j++) {
            const TagConfig *t = &s->tags[j];
            uint16_t index = 0;
            uint8_t subindex = 0;

            parse_pdo_entry_addr(t->address, &index, &subindex);

            printf("  [%d] %-16s %-12s -> index 0x%04X sub 0x%02X  %2d bit"
                   "  |  配置 offset=%d  IgH 分配 offset=%u\n",
                   j, t->name, t->address, index, subindex,
                   raw_type_bit_length(t->raw_type),
                   t->offset, conn.tag_offset[i][j]);
        }
    }
}

/* ============================================================================
 *  构建（只能在 Linux 上做，Windows 没有 ecrt.h / libethercat）：
 *
 *    gcc -Wall -g -I<test-file> -I<ethercat-1.6-stable>/include \
 *        ethercat_south_connect.c ethercat_south_parse.c cJSON/cJSON.c \
 *        -o connect_test -lethercat
 *
 *    sudo ./connect_test
 *      需要 root 权限 + 已加载 ethercat 内核模块 + 真实从站。
 *
 *  已知缺口：
 *    1. JSON 里的 product_code 目前是占位值，真实值要从从站的 ESI 文件取。
 *    2. 从站必须在总线上，否则 ecrt_slave_config_reg_pdo_entry() 会失败。
 *    3. 中间任何一步失败都直接返回 -1，没有 release 主站，真实模块需要逐点清理。
 * ============================================================================ */
