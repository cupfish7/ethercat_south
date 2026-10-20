#ifndef ETHERCAT_SOUTH_PARSE_H
#define ETHERCAT_SOUTH_PARSE_H

#include "cJSON/cJSON.h"
#include <stdint.h>

#define MAX_TAGS        64
#define MAX_START_SDO   32
#define MAX_SDO_DATA    32

/* JSON 里的 raw_type / value_type / data_type 统一到这个枚举。
   0 固定为 UNKNOWN：Config 被 memset 后，未解析的字段可以被识别为无效。 */
typedef enum {
    SOUTH_TYPE_UNKNOWN = 0,
    SOUTH_TYPE_BOOL,
    SOUTH_TYPE_I8,  SOUTH_TYPE_U8,
    SOUTH_TYPE_I16, SOUTH_TYPE_U16,
    SOUTH_TYPE_I32, SOUTH_TYPE_U32,
    SOUTH_TYPE_I64, SOUTH_TYPE_U64,
    SOUTH_TYPE_F32,          /* JSON: real */
    SOUTH_TYPE_F64           /* JSON: lreal */
} south_data_type_t;

typedef enum {
    SOUTH_DIR_UNKNOWN = 0,
    SOUTH_DIR_IN,
    SOUTH_DIR_OUT
} south_direction_t;

typedef enum {
    SOUTH_MODE_UNKNOWN = 0,
    SOUTH_MODE_FREERUN,
    SOUTH_MODE_SM,
    SOUTH_MODE_DC
} south_sync_mode_t;

typedef struct {
    char              name[64];
    char              address[32];   /* 原始文本，便于日志/回写 */
    uint16_t          index;         /* 从 address 解析，运行时寻址用 */
    uint8_t           subindex;
    south_data_type_t raw_type;      /* 过程映像里怎么读 */
    south_data_type_t value_type;    /* 输出帧 union 里写哪个成员 */
    south_direction_t direction;
    int               json_offset;   /* JSON 里的参考偏移，运行时不用 */
    double            scale;         /* 当前不参与换算，保留给下游 */
    char              unit[16];
} TagConfig;

typedef struct {
    uint16_t assign_activate;
    uint32_t sync0_cycle_ns;
    int32_t  sync0_shift_ns;
    uint32_t sync1_cycle_ns;
    int32_t  sync1_shift_ns;         /* IgH 1.6 会忽略这个值 */
} DcConfig;

typedef struct {
    char              address[32];   /* 原始文本 */
    uint16_t          index;
    uint8_t           subindex;
    south_data_type_t data_type;
    uint8_t           data[MAX_SDO_DATA]; /* data_hex 解析后的原始字节 */
    uint8_t           data_len;
    int               apply;         /* 1 = 启动配置阶段下发 */
} StartSdoConfig;

typedef struct {
    char      prefix[64];
    char      protocol[32];
    char      node[64];

    uint16_t  alias;                 /* 替代原来的 slave[64] */
    uint16_t  position;
    uint32_t  vendor_id;
    uint32_t  product_code;

    south_sync_mode_t mode;
    int       period_ms;

    DcConfig  dc;
    int       sdo_count;
    StartSdoConfig sdo[MAX_START_SDO];

    char      publish[16];
    int       tag_count;
    TagConfig tags[MAX_TAGS];
} SlaveConfig;

#define MAX_SLAVES 128
typedef struct {
    int        slave_count;
    SlaveConfig slaves[MAX_SLAVES];
} Config;

/* 类型枚举 → JSON 里的规范名字（日志/打印用）。 */
const char *south_type_name(south_data_type_t type);

/* 同步模式枚举 → JSON 里的规范名字（日志/打印用）。 */
const char *south_mode_name(south_sync_mode_t mode);

/* 类型宽度。返回 0 表示未知类型。 */
int south_type_bit_length(south_data_type_t type);
int south_type_width_bytes(south_data_type_t type);

/* 解析单个 tag 对象。严格模式：必填字段缺失/类型错/地址非法都返回 -1。 */
int parse_tag(cJSON *tag_json, TagConfig *tag);

/* 解析单个从站对象。
   protocol != "ethercat" 时只填公共字段并返回 0，调用方负责丢弃。
   ethercat 从站按严格模式解析，任何错误返回 -1。 */
int parse_slave(cJSON *slave_json, SlaveConfig *slave);

/* 遍历 dirname 下所有 .json 文件，把解析出的 ethercat 从站累加到 config。
   非 ethercat 从站直接丢弃。
   文件名排序后处理，保证 Config.slaves[] 的顺序稳定。
   任何一个文件出错（打不开/语法错/字段错）都返回 -1，不再“跳过继续”。
   config->slave_count 会被先清零。
   返回 0 成功；-1 失败。 */
int configuration_store_load_dir(const char *dirname, Config *config);

/* 完成整个 JSON 配置结构体化：加载 "JSON" 目录。
   返回 0 成功；-1 失败（目录打不开、任一文件错、或没有任何 ethercat 从站）。 */
int configuration_store_load_startup(Config *config);

/* 测试/演示：加载配置并打印结构体内容。 */
void configuration_store_load_test(void);

#endif /* ETHERCAT_SOUTH_PARSE_H */
