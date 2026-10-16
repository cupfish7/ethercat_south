#ifndef ETHERCAT_SOUTH_READ_H
#define ETHERCAT_SOUTH_READ_H

#include "ethercat_south_connect.h"      /* 要 ethercat_south_conn_t */

typedef struct {
    char     address[64];        ///< 原始地址（如 holding:40001、0x6064:00）
    char     raw_type[16];       ///< 原始类型（u16/u32/i32/...）
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
    } value;             ///现在为8字节，足够应付绝大部分场景，如需再后续调整
} south_tag_t;

typedef struct {
    uint64_t frame_sequence;     ///< 该从站的第几帧（按 node_id 各自计数），跳号即丢帧
    uint64_t source_received_at_ms;
    int32_t      node_id;      //node_id用于在Startsetup Configution中的寻址
    char     source_protocol[32];
    uint32_t   tag_count;
    south_tag_t tags[64];   ///固定内存，后续可尝试优化
} south_frame_t;

/* 读一个从站、封装成一帧。

   只从过程映像搬字节，**不驱动任何周期** ——
   ecrt_master_receive / ecrt_domain_process / ecrt_domain_queue / ecrt_master_send
   由调用方（将来的 collector）负责。

   conn          ：ethercat_south_master_start() 填好的连接状态
   slave_index   ：Config.slaves[] 的下标。它同时直接作为输出帧的 node_id
   received_at_ms：本帧的采样时刻 [ms]。由周期驱动方在 ecrt_master_receive() 之后
                   打戳传进来 —— read 自己不取时钟，所以它是无时钟依赖的纯函数、可单测
   out           ：输出帧，由调用方分配

   输出帧的 tags[j] 与 config->slaves[slave_index].tags[j] **一一对应（同下标）**；
   某个 tag 的 raw_type / value_type 不认识时，该槽位 value 置 0 并打印一条警告，
   **不压缩数组** —— 下游可以安心的按下标和配置对上号。

   frame_sequence 的语义：**该从站的第几帧**（每个从站各自计数，从 1 开始），
   所以跳号就意味着丢帧。据此：
     · 要全局唯一的键 → 用 (node_id, frame_sequence)
     · 要全局时序     → 用 source_received_at_ms
   同一个从站连续两帧的 frame_sequence 一定连续；不同从站之间互不影响。

   转换规则：按 raw_type 读原始值，按 value_type 强转后存进 union，**不乘 scale**。

   返回 0 成功；-1 参数非法 / 该从站没有域 / 过程映像尚不可用（还没 activate）。 */
int ethercat_south_read_node(const ethercat_south_conn_t *conn,
                             int slave_index,
                             uint64_t received_at_ms,
                             south_frame_t *out);

#endif /* ETHERCAT_SOUTH_READ_H */
