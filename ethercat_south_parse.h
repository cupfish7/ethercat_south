#ifndef ETHERCAT_SOUTH_PARSE_H
#define ETHERCAT_SOUTH_PARSE_H

#include "cJSON/cJSON.h"
#include <stdint.h>

#define MAX_TAGS 64

typedef struct {
    char address[32];
    char raw_type[16];
    char value_type[16];
    char direction[8];
    int offset;
    char unit[16];
    double scale;
    char name[64];
} TagConfig;

typedef struct {
    char prefix[64];
    char protocol[32];
    char node[64];
    char slave[64];
    uint32_t vendor_id;      /* EtherCAT 从站厂商 ID，JSON 里写 "0x00000002" */
    uint32_t product_code;   /* EtherCAT 从站产品码，JSON 里写 "0x07D43052" */
    int period_ms;
    char publish[16];

    int tag_count;
    TagConfig tags[MAX_TAGS];
} SlaveConfig;

#define MAX_SLAVES 128
typedef struct {
    int slave_count;
    SlaveConfig slaves[MAX_SLAVES];
} Config;

/* 解析单个 tag 对象，字段缺失保持默认值，字段类型不对返回 -1 */
int parse_tag(cJSON *tag_json, TagConfig *tag);

/* 解析单个从站对象，字段缺失保持默认值，字段类型不对返回 -1 */
int parse_slave(cJSON *slave_json, SlaveConfig *slave);

/* 遍历 dirname 下所有 .json 文件，把解析出的从站累加到 config。
   只保留 protocol == "ethercat" 的从站，其它协议直接丢弃。
   单个文件出错（打不开/语法错/字段错）就跳过，继续处理其余文件。
   config->slave_count 会被先清零。
   返回 0 表示目录已遍历；-1 表示目录打不开。 */
int configuration_store_load_dir(const char *dirname, Config *config);

/* 完成整个 JSON 配置结构体化：把配置目录下的 ethercat 配置解析进 config。
   只保留 protocol == "ethercat" 的从站。
   不打印配置内容，加载过程中的错误提示仍会打印。
   config->slave_count 会被先清零。
   返回 0 表示目录已遍历；-1 表示目录打不开。 */
int configuration_store_load_startup(Config *config);

/* 测试/演示：调用 configuration_store_load_startup 并打印配置结构体内容。 */
void configuration_store_load_test(void);

#endif /* ETHERCAT_SOUTH_PARSE_H */
