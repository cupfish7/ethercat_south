/* main.c —— 南向模块的项目主函数
 *
 * 组装链条（collector 内部把 parse / connect / read 串起来）：
 *
 *     configuration_store_load_startup()     JSON → Config
 *              │
 *              ▼
 *     ethercat_south_collector_start()       建主站 → 按采样周期建域 → 注册 PDO
 *                                            → 激活 → 取过程映像 → 初始化调度
 *              │
 *              ▼
 *     ethercat_south_collector_run()         按各域自己的周期分频读，死循环
 *
 * 只能在 Linux 上跑（要 libethercat，而且要 root —— ecrt_request_master 需要）。
 * Windows 上只能做语法检查，见 .vscode/tasks.json 的「语法检查（全部模块）」任务。
 */

#include <stdio.h>

#include "ethercat_south_parse.h"
#include "ethercat_south_collector.h"

static Config config;                          /* 约 1.34 MB，必须离开栈 */
static ethercat_south_collector_t collector;   /* 约 40 KB，同样必须离开栈 */

int main(void)
{
    int rc;

    /* 第一步：解析 JSON 配置（parse 模块，通用工具，别的主站程序也能用） */
    if (configuration_store_load_startup(&config) != 0) {
        printf("配置加载失败\n");
        return 1;
    }

    /* 第二步：组装并启动（collector 内部会调 connect） */
    if (ethercat_south_collector_start(&collector, &config) != 0) {
        printf("collector 启动失败\n");
        return 1;
    }

    /* 第三步：跑周期循环。
       调试时换成下面这行（0 = 自动取各域周期的最大公约数当拍长）：
           return ethercat_south_collector_run_debug(&collector, 0);
       需要自己控循环（比如要优雅退出）就用 _tick()：
           while (keep_running) ethercat_south_collector_tick(&collector,
                                       ethercat_south_collector_now_ms());  */
    rc = ethercat_south_collector_run(&collector);

    /* 第四步：收尾 —— 释放主站。
       Ctrl+C / SIGTERM 让 _run() 退出之后**必须调这一步**。
       ⚠️ SIGKILL（kill -9）是收不到的，那种情况下这一步不会执行。 */
    ethercat_south_collector_stop(&collector);

    return rc;
}
