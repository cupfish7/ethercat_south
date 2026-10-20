/* ============================================================================
 *  ethercat_south_read.c —— 把 EtherCAT 过程映像里的字节转成 south_frame_t
 *
 *  职责：读一个从站、封装成一帧。
 *
 *  不做的事（都是有意的，别顺手加）：
 *    · 不驱动周期 —— receive / domain_process / domain_queue / master_send 归调用方
 *    · 不乘 scale —— 只发原始数据
 *    · 不写输出 —— direction=="out" 的槽位固定置 0
 *    · 不推送队列 —— 那是 collector 的事
 *
 *  帧与 Config 的契约见 read.h 顶部注释。
 * ============================================================================ */

#include <stdio.h>
#include <string.h>

#include "ethercat_south_read.h"
#include "ecrt.h"      /* EC_READ_* 宏 */

/* 每个从站一个帧序列。read 不做周期调度，所以对某个从站而言，
   这个编号就是"它被读到的第几帧" —— 跳号即丢帧。
   （128 × 8 字节 = 1 KB BSS） */
static uint64_t g_frame_sequence[MAX_SLAVES];

/* ---------- 原始值的中间表示 ---------- */

typedef enum { RAW_UINT, RAW_SINT, RAW_FLOAT } raw_kind_t;

/* 归一后的原始值。64 位整数不能过 double，所以整数留在整数载体里。 */
typedef struct {
    raw_kind_t kind;
    union {
        int64_t  i;
        uint64_t u;
        double   d;
    } v;
} raw_value_t;

/* 按 raw_type 从过程映像读出原始值。
   返回 0 成功；-1 表示类型未知。 */
static int read_raw(const uint8_t *p, south_data_type_t raw_type,
                    raw_value_t *out)
{
    switch (raw_type) {
    case SOUTH_TYPE_BOOL:
    case SOUTH_TYPE_U8:
        out->kind = RAW_UINT;
        out->v.u = EC_READ_U8(p);
        return 0;

    case SOUTH_TYPE_I8:
        out->kind = RAW_SINT;
        out->v.i = EC_READ_S8(p);
        return 0;

    case SOUTH_TYPE_U16:
        out->kind = RAW_UINT;
        out->v.u = EC_READ_U16(p);
        return 0;

    case SOUTH_TYPE_I16:
        out->kind = RAW_SINT;
        out->v.i = EC_READ_S16(p);
        return 0;

    case SOUTH_TYPE_U32:
        out->kind = RAW_UINT;
        out->v.u = EC_READ_U32(p);
        return 0;

    case SOUTH_TYPE_I32:
        out->kind = RAW_SINT;
        out->v.i = EC_READ_S32(p);
        return 0;

    case SOUTH_TYPE_U64:
        out->kind = RAW_UINT;
        out->v.u = EC_READ_U64(p);
        return 0;

    case SOUTH_TYPE_I64:
        out->kind = RAW_SINT;
        out->v.i = EC_READ_S64(p);
        return 0;

    case SOUTH_TYPE_F32: {
        /* 用位模式搬运，不依赖 ecrt_read_real() 的链接依赖。 */
        uint32_t bits = EC_READ_U32(p);
        float f;
        memcpy(&f, &bits, sizeof(f));
        out->kind = RAW_FLOAT;
        out->v.d = (double)f;
        return 0;
    }

    case SOUTH_TYPE_F64: {
        uint64_t bits = EC_READ_U64(p);
        double d;
        memcpy(&d, &bits, sizeof(d));
        out->kind = RAW_FLOAT;
        out->v.d = d;
        return 0;
    }

    default:
        return -1;
    }
}

static double raw_to_double(const raw_value_t *rv)
{
    if (rv->kind == RAW_FLOAT) return rv->v.d;
    if (rv->kind == RAW_SINT)  return (double)rv->v.i;
    return (double)rv->v.u;
}

static int64_t raw_to_i64(const raw_value_t *rv)
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
static int store_as(south_tag_t *tag, south_data_type_t value_type,
                    const raw_value_t *rv)
{
    /* f32/f64 是 union 的不同成员，且都小于 8 字节 —— 先清干净再写。 */
    memset(&tag->value, 0, sizeof(tag->value));

    switch (value_type) {
    case SOUTH_TYPE_U8:  tag->value.u8  = (uint8_t) raw_to_u64(rv);  return 0;
    case SOUTH_TYPE_U16: tag->value.u16 = (uint16_t)raw_to_u64(rv);  return 0;
    case SOUTH_TYPE_U32: tag->value.u32 = (uint32_t)raw_to_u64(rv);  return 0;
    case SOUTH_TYPE_U64: tag->value.u64 = raw_to_u64(rv);            return 0;

    case SOUTH_TYPE_I8:  tag->value.i8  = (int8_t)  raw_to_i64(rv);  return 0;
    case SOUTH_TYPE_I16: tag->value.i16 = (int16_t) raw_to_i64(rv);  return 0;
    case SOUTH_TYPE_I32: tag->value.i32 = (int32_t) raw_to_i64(rv);  return 0;
    case SOUTH_TYPE_I64: tag->value.i64 = raw_to_i64(rv);            return 0;

    case SOUTH_TYPE_F32: tag->value.f32 = (float)  raw_to_double(rv); return 0;
    case SOUTH_TYPE_F64: tag->value.f64 = raw_to_double(rv);          return 0;

    default:
        return -1;
    }
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
    if (conn->domain_pd == NULL) return -1;      /* 还没 activate */
    if (conn->domain_size == 0) return -1;

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
        raw_value_t      rv;
        int              off;
        int              width;

        strncpy(dst->address, t->address, sizeof(dst->address) - 1);
        dst->raw_type = t->raw_type;

        /* 输出条目不参与采集：保留槽位，value 保持 0。
           它们仍然注册进 domain，是为了让对应 SM 留在周期数据里。 */
        if (t->direction == SOUTH_DIR_OUT) continue;

        off   = conn->tag_offset[slave_index][j];
        width = south_type_width_bytes(t->raw_type);

        if (off < 0 || width <= 0 ||
            (size_t)off + (size_t)width > conn->domain_size) {
            printf("[%s.%s] 偏移不可用（offset=%d width=%d domain=%lu），该值置 0\n",
                   s->node, t->name, off, width,
                   (unsigned long)conn->domain_size);
            continue;
        }

        if (read_raw(base + off, t->raw_type, &rv) != 0 ||
            store_as(dst, t->value_type, &rv) != 0) {
            memset(&dst->value, 0, sizeof(dst->value));
            printf("[%s.%s] raw_type=%s value_type=%s 不支持，该值置 0\n",
                   s->node, t->name, south_type_name(t->raw_type),
                   south_type_name(t->value_type));
        }
    }

    return 0;
}
