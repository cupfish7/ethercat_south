#ifndef ETHERCAT_SOUTH_READ_H
#define ETHERCAT_SOUTH_READ_H

#include "ethercat_south_connect.h"      /* 要 ethercat_south_conn_t / Config */

typedef struct {
    char              address[64];   ///< 原始地址（如 "0x6064:00"）
    south_data_type_t raw_type;      ///< 原始读取类型
    union {
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        int8_t   i8;
        int16_t  i16;
        int32_t  i32;
        int64_t  i64;
        float    f32;
        double   f64;
    } value;
} south_tag_t;

typedef struct {
    uint64_t frame_sequence;     ///< 该从站的第几帧（按从站各自计数），跳号即丢帧
    uint64_t source_received_at_ms;
    int32_t  node_id;            ///< Config.slaves[] 的下标
    char     source_protocol[32];
    uint32_t tag_count;
    south_tag_t tags[64];        ///< 固定内存
} south_frame_t;

/* ============================================================================
 *  帧与配置的契约（重要）
 *
 *  south_frame_t 不是自描述的。使用一帧时，必须同时持有产生它的 Config：
 *
 *    - frame.node_id 是 Config.slaves[] 的下标；
 *    - frame.tags[j] 与 Config.slaves[node_id].tags[j] 一一对应（同下标）；
 *    - frame.tag_count == Config.slaves[node_id].tag_count；
 *    - value 里哪个 union 成员有效，由 Config 的 value_type 决定；
 *      frame.tags[j].raw_type 只说明原始字节是怎么读出来的；
 *    - 未注册、类型不支持、或 direction=="out" 的槽位保留并置 0，
 *      **不压缩数组**；下游不能靠“跳过空槽”来对齐配置；
 *    - direction=="out" 的条目只注册进 domain，当前不写输出，帧里固定为 0。
 *
 *  因此下游必须使用同一份 Config（或其快照），不能只凭 frame 解释 value。
 * ========================================================================== */

/* 读一个从站、封装成一帧。
 *
 * 只从过程映像搬字节，**不驱动任何周期** ——
 * ecrt_master_receive / ecrt_domain_process / ecrt_domain_queue / ecrt_master_send
 * 由调用方（collector）负责。
 *
 * conn          ：ethercat_south_master_start() 填好的连接状态
 * slave_index   ：Config.slaves[] 的下标；同时作为输出帧的 node_id
 * received_at_ms：本帧的采样时刻 [ms]，由周期驱动方在 receive 之后打戳
 * out           ：输出帧，由调用方分配
 *
 * 输出帧的 tags[j] 与 config->slaves[slave_index].tags[j] **一一对应（同下标）**。
 * direction=="out" 的槽位固定为 0；其它槽位读取失败/类型不认识时置 0 并打印警告，
 * **不压缩数组**。
 *
 * 返回 0 成功；-1 参数非法 / 过程映像尚不可用（还没 activate）。 */
int ethercat_south_read_node(const ethercat_south_conn_t *conn,
                             int slave_index,
                             uint64_t received_at_ms,
                             south_frame_t *out);

#endif /* ETHERCAT_SOUTH_READ_H */
