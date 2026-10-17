#ifndef ETHERCAT_SOUTH_CONNECT_H
#define ETHERCAT_SOUTH_CONNECT_H

#include "ethercat_south_parse.h"      /* 用到 Config / SlaveConfig / TagConfig */
#include "ethercat_south_l6n_control.h"/* L6N 伺服控制（配置期那两件事在 connect 里做） */
#include "ecrt.h"                      /* conn 里有 ec_master_t / ec_domain_t */

/* 主站连接与启动的产出，由调用方持有。
   约 33 KB（光 tag_offset[128][64] 就 32 KB）—— 所以只能调用方分配、传指针，
   不能按值返回，也不能放栈上。

   生命周期约定：调 ethercat_south_master_start() 填好，之后 read 只读它。 */
typedef struct {
    const Config *config;                            /* 从站元数据（tag 类型等），read 要用 */

    ec_master_t  *master;                            /* 请求到的主站句柄 */
    ec_domain_t  *domain;                            /* **唯一的域**：所有从站的过程数据都在里面 */
    uint8_t      *domain_pd;                         /* 过程映像基址，activate 之后才有 */

    unsigned int  tag_offset[MAX_SLAVES][MAX_TAGS];  /* tag j 在 domain_pd 里的字节偏移 */

    /* L6N 伺服控制的状态（CiA402 状态机 + 运动参数）。
       配置期由 ethercat_south_master_start() 填好，周期期由 collector 每拍调 _step()。
       ⚠️ 会让电机真的转，见 ethercat_south_l6n_control.h。 */
    ethercat_south_l6n_t l6n[MAX_SLAVES];
} ethercat_south_conn_t;

/* 按配置启动 EtherCAT 主站：请求主站 → 按采样周期建域 → 注册 PDO 条目 →
   激活 → 取各自的过程映像。结果全部写进 conn。
   protocol != "ethercat" 的从站会被跳过。
   返回 0 成功；-1 失败（失败时不自动 release 主站，由调用方决定）。 */
int ethercat_south_master_start(const Config *config, ethercat_south_conn_t *conn);

/* 测试/演示：加载 JSON 配置 → ethercat_south_master_start() → 打印域清单与过程映像偏移。 */
void ethercat_south_connect_test(void);

#endif /* ETHERCAT_SOUTH_CONNECT_H */
