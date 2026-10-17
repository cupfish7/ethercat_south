/* ============================================================================
 *  ethercat_south_read.c —— 把 EtherCAT 过程映像里的字节转成 south_frame_t
 *
 *  职责：读一个从站、封装成一帧。
 *
 *  不做的事（都是有意的，别顺手加）：
 *    · 不驱动周期 —— receive / domain_process / domain_queue / master_send 归调用方
 *    · 不乘 scale —— 只发原始数据
 *    · 不推送队列 —— 那是 collector 的事
 *
 *  步骤实现图
 *  ----------
 *
 *    [ethercat_south_conn_t]              ← connect 产出，调用方持有
 *          │  slave_index
 *          ▼
 *    ①  conn->domain_pd                   → 过程映像基址（只有一个域）
 *          ▼
 *    ③  逐 tag j：
 *          conn->tag_offset[slave_index][j]     → 该条目在过程映像里的字节偏移
 *          按 t->raw_type   选 EC_READ_* 读出原始值
 *          按 t->value_type 强转后写进 south_tag_t.value（不乘 scale）
 *          ▼
 *    [south_frame_t]                      ← 输出
 *
 *  输入与输出的对应关系
 *  --------------------
 *    Config.slaves[i]（下标 i）        →  south_frame_t.node_id
 *    SlaveConfig.protocol              →  south_frame_t.source_protocol
 *    SlaveConfig.tag_count / tags[j]   →  south_frame_t.tag_count / tags[j]（同下标）
 *    TagConfig.address / raw_type      →  south_tag_t.address / raw_type
 *    TagConfig.value_type              →  决定 value 里写哪个 union 成员
 *    TagConfig.scale                   →  【不使用】只发原始数据
 * ============================================================================ */

#include <stdio.h>
#include <string.h>

#include "ethercat_south_read.h"
#include "ecrt.h"      /* EC_READ_* 宏 */

/* 每个从站一个帧序列。read 不做周期调度，所以对某个从站而言，
   这个编号就是"它被读到的第几帧" —— 跳号即丢帧。
   （128 × 8 字节 = 1 KB BSS）

   下标安全性：read_node 一开头已经判过 slave_index < config->slave_count，
   而 slave_count <= MAX_SLAVES（Config.slaves[] 就是 MAX_SLAVES 长），
   所以这里不会越界，不需要再加判断。 */
static uint64_t g_frame_sequence[MAX_SLAVES];

/* ---------- 原始值的中间表示 ---------- */

/* 原始值归一后的种类。整数区分有无符号，浮点统一进 double。 */
typedef enum { RAW_UINT, RAW_SINT, RAW_FLOAT } raw_kind_t;

/* 归一后的原始值。一次读取只可能是三类之一，kind 说明哪个成员有效。

   为什么要三类、而不是统一用一个 double 装：**64 位整数不能过 double** ——
   double 只有 53 位有效位，i64 / u64 超过 2^53 的值会静默丢精度。
   所以整数必须留在 64 位整数载体里，浮点单独走 double。 */
typedef struct {
    raw_kind_t kind;
    union {
        int64_t  i;     /* raw_type 是有符号整数（i8/i16/i32/i64）时有效 */
        uint64_t u;     /* raw_type 是无符号整数（u8/u16/u32/u64/bool）时有效 */
        double   d;     /* raw_type 是 real / lreal 时有效 */
    } v;
} raw_value_t;

/* 按 raw_type 从过程映像读出原始值，归一成 (kind, 值)。
   返回 0 成功；-1 表示 raw_type 不认识。

   注意 real / lreal 刻意**不用** EC_READ_REAL / EC_READ_LREAL ——
   那两个在用户态是 libethercat 里的函数（ecrt_read_real / ecrt_read_lreal），
   一用就产生链接依赖、本机就测不了了。这里改用 EC_READ_U32/U64 搬位模式，
   效果一样，而且 read.c 因此**不引用任何 ecrt_* 库函数**。 */
static int read_raw(const uint8_t *p, const char *raw_type, raw_value_t *out)
{
    if (strcmp(raw_type, "bool") == 0 || strcmp(raw_type, "u8") == 0) {
        out->kind = RAW_UINT; out->v.u = EC_READ_U8(p);  return 0;
    }
    if (strcmp(raw_type, "i8") == 0) {
        out->kind = RAW_SINT; out->v.i = EC_READ_S8(p);  return 0;
    }
    if (strcmp(raw_type, "u16") == 0) {
        out->kind = RAW_UINT; out->v.u = EC_READ_U16(p); return 0;
    }
    if (strcmp(raw_type, "i16") == 0) {
        out->kind = RAW_SINT; out->v.i = EC_READ_S16(p); return 0;
    }
    if (strcmp(raw_type, "u32") == 0) {
        out->kind = RAW_UINT; out->v.u = EC_READ_U32(p); return 0;
    }
    if (strcmp(raw_type, "i32") == 0) {
        out->kind = RAW_SINT; out->v.i = EC_READ_S32(p); return 0;
    }
    if (strcmp(raw_type, "u64") == 0) {
        out->kind = RAW_UINT; out->v.u = EC_READ_U64(p); return 0;
    }
    if (strcmp(raw_type, "i64") == 0) {
        out->kind = RAW_SINT; out->v.i = EC_READ_S64(p); return 0;
    }
    if (strcmp(raw_type, "real") == 0) {
        uint32_t bits = EC_READ_U32(p);
        float f;
        memcpy(&f, &bits, sizeof(f));               /* 位模式搬运，不做类型双关 */
        out->kind = RAW_FLOAT; out->v.d = (double)f; return 0;
    }
    if (strcmp(raw_type, "lreal") == 0) {
        uint64_t bits = EC_READ_U64(p);
        double dd;
        memcpy(&dd, &bits, sizeof(dd));
        out->kind = RAW_FLOAT; out->v.d = dd;       return 0;
    }
    return -1;
}

/* 归一后的原始值 → 各目标数值类型。浮点转整数按 C 的截断语义。 */
static double   raw_to_double(const raw_value_t *rv)
{
    if (rv->kind == RAW_FLOAT) return rv->v.d;
    if (rv->kind == RAW_SINT)  return (double)rv->v.i;
    return (double)rv->v.u;
}
static int64_t  raw_to_i64(const raw_value_t *rv)
{
    if (rv->kind == RAW_FLOAT) return (int64_t)rv->v.d;
    if (rv->kind == RAW_SINT)  return rv->v.i;
    return (int64_t)rv->v.u;
}
static uint64_t raw_to_u64(const raw_value_t *rv)
{
    if (rv->kind == RAW_FLOAT) return (uint64_t)rv->v.d;
    if (rv->kind == RAW_SINT)  return (uint64_t)rv->v.i;
    return rv->v.u;
}

/* 按 value_type 把归一后的原始值写进 union 的对应成员。**不乘 scale。**
   返回 0 成功；-1 表示 value_type 不认识。 */
static int store_as(south_tag_t *tag, const char *value_type, const raw_value_t *rv)
{
    /* f32 和 f64 是 union 的不同成员，且都小于 8 字节 —— 不先清干净会留下高位残渣 */
    memset(&tag->value, 0, sizeof(tag->value));

    if      (strcmp(value_type, "u8")  == 0) tag->value.u8  = (uint8_t) raw_to_u64(rv);
    else if (strcmp(value_type, "u16") == 0) tag->value.u16 = (uint16_t)raw_to_u64(rv);
    else if (strcmp(value_type, "u32") == 0) tag->value.u32 = (uint32_t)raw_to_u64(rv);
    else if (strcmp(value_type, "u64") == 0) tag->value.u64 = raw_to_u64(rv);
    else if (strcmp(value_type, "i8")  == 0) tag->value.i8  = (int8_t)  raw_to_i64(rv);
    else if (strcmp(value_type, "i16") == 0) tag->value.i16 = (int16_t) raw_to_i64(rv);
    else if (strcmp(value_type, "i32") == 0) tag->value.i32 = (int32_t) raw_to_i64(rv);
    else if (strcmp(value_type, "i64") == 0) tag->value.i64 = raw_to_i64(rv);
    else if (strcmp(value_type, "f32") == 0) tag->value.f32 = (float)  raw_to_double(rv);
    else if (strcmp(value_type, "f64") == 0) tag->value.f64 = raw_to_double(rv);
    else return -1;

    return 0;
}

/* ---------- 对外接口 ---------- */

int ethercat_south_read_node(const ethercat_south_conn_t *conn,
                             int slave_index,
                             uint64_t received_at_ms,
                             south_frame_t *out)
{
    const SlaveConfig *s;
    uint8_t *base;
    int j;

    /* ---------- 前置条件 ---------- */
    if (conn == NULL || conn->config == NULL || out == NULL) return -1;
    if (slave_index < 0 || slave_index >= conn->config->slave_count) return -1;
    if (conn->domain_pd == NULL) return -1;            /* 还没 activate，过程映像还没映射 */

    s    = &conn->config->slaves[slave_index];
    base = conn->domain_pd;

    /* ---------- 帧头 ---------- */
    memset(out, 0, sizeof(*out));
    out->frame_sequence        = ++g_frame_sequence[slave_index];
    out->source_received_at_ms = received_at_ms;
    out->node_id               = (int32_t)slave_index;
    strncpy(out->source_protocol, s->protocol, sizeof(out->source_protocol) - 1);

    /* ---------- 逐 tag 取数 ---------- */
    out->tag_count = (uint32_t)s->tag_count;

    for (j = 0; j < s->tag_count && j < MAX_TAGS; j++) {
        const TagConfig *t   = &s->tags[j];
        south_tag_t     *dst = &out->tags[j];
        raw_value_t rv;

        strncpy(dst->address,  t->address,  sizeof(dst->address)  - 1);
        strncpy(dst->raw_type, t->raw_type, sizeof(dst->raw_type) - 1);

        /* 按 raw_type 读原始值 → 按 value_type 转换，不乘 scale。
           任何一步不认识就置 0 + 警告，但**槽位保留** —— 这样 out->tags[j]
           永远对应 config->slaves[i].tags[j]，下游能按下标对上号。 */
        if (read_raw(base + conn->tag_offset[slave_index][j], t->raw_type, &rv) != 0 ||
            store_as(dst, t->value_type, &rv) != 0) {
            memset(&dst->value, 0, sizeof(dst->value));
            printf("[%s.%s] raw_type=\"%s\" value_type=\"%s\" 不支持，该值置 0\n",
                   s->node, t->name, t->raw_type, t->value_type);
        }
    }

    return 0;
}
