/* Windows 专用垫片。
 *
 * ecrt.h:152 要 #include <netinet/in.h>（只为拿 struct in_addr，给 EoE 配置用），
 * 而 MinGW 的 include 目录里没有 netinet/ 这个目录 —— 于是任何引用 ecrt.h 的
 * 文件在本机都编不过。
 *
 * 这个文件只为了补上那个头。Linux 上永远不会用到它
 * （glibc 自带 netinet/in.h，-Icompat 排不到前面，而且路径也不冲突）。
 *
 * 注意：它不是 ecrt.h 的一部分，不要把它塞进 ethercat-1.6-stable/ ——
 * 那是上游源码树，要保持零改动、方便整体替换。
 */
#include <winsock2.h>
