#ifndef ETHERCAT_SOUTH_CONNECT_H
#define ETHERCAT_SOUTH_CONNECT_H

#include <stddef.h>

#include "ethercat_south_parse.h"      /* Config / SlaveConfig / TagConfig */
#include "ecrt.h"                      /* ec_master_t / ec_domain_t */

/* 主站连接与启动的产出，由调用方持有。
   约 32 KB（光 tag_offset[128][64] 就 32 KB）—— 只能调用方分配、传指针，
   不能按值返回，也不能放栈上。

   生命周期约定：调 ethercat_south_master_start() 填好，之后 read 只读它。 */
typedef struct {
    const Config *config;                            /* 从站元数据（tag 类型等） */

    ec_master_t  *master;                            /* 请求到的主站句柄 */
    ec_domain_t  *domain;                            /* **唯一的域** */
    uint8_t      *domain_pd;                         /* 过程映像基址，activate 之后才有 */
    size_t        domain_size;                       /* activate 之后读取 */

    int           tag_offset[MAX_SLAVES][MAX_TAGS];  /* -1 = 未注册；否则是 domain 内字节偏移 */
} ethercat_south_conn_t;

/* 按配置启动 EtherCAT 主站：
   request_master → 建唯一 domain → 每个从站配置 SDO/DC/PDO → activate →
   取过程映像并清零 → 记录 domain 大小。

   任何从站配置失败（含条目不在默认映射、从站不在线）都会让整个启动失败；
   失败时已释放主站并把 conn->master 置 NULL。
   返回 0 成功；-1 失败。 */
int ethercat_south_master_start(const Config *config, ethercat_south_conn_t *conn);

/* 测试/演示：加载 JSON → 启动主站 → 打印域大小、从站和 tag 偏移。 */
void ethercat_south_connect_test(void);

#endif /* ETHERCAT_SOUTH_CONNECT_H */
