# 雷赛 L6N 伺服（EtherCAT 从站）笔记

> 这份文档记录**这台从站**实测出来的东西，供后续继续做这个从站时少走弯路。
> 全部数据来自 2026-10-09 在 openEuler 工控机（`192.168.100.104`）上的实跑。
> 驱动它的代码在 `test-file/ethercat_south_l6n_control.{c,h}`。

---

## 0. 一句话

一台 **CiA402 伺服**，IgH 主站下能跑通完整的使能序列并做 CSP 位置跟随。
**编码器 22 位**（4,194,304 counts/转）—— 这个数字是理解它一切"怪现象"的钥匙。
**不支持 SDO Info**，所以 `ethercat` 命令行工具**必须显式给 `-t`**。

---

## 1. 身份

| 项 | 值 |
|---|---|
| **厂商 ID** | `0x00004321` |
| **产品码** | `0x000000e2` |
| 修订号 | `0x00000001` |
| 站地址 | `0/0`（alias 0, position 0） |
| 设备名 | `L6N(COE)` |
| 组 | Servo Drive |
| 支持协议 | CoE, FoE |
| **SDO Info** | **不支持**（`Enable SDO Info: no`）← 见 §4 |
| 分布式时钟 | 支持，64 位；主站把 Slave 0 当参考时钟 |
| 邮箱 | Std RX `0x1000/124`，TX `0x1100/124`；Bootstrap RX `0x1000/124`，TX `0x1100/128` |

> ⚠️ `aiworkplace/SUHUAN/ethercat_l6n_working.c` 那份参考实现里写的
> `VENDOR_ID 0x000000dd` / `PRODUCT_CODE 0x00007595`（注释标 InovanceAutomation）
> **是另一台设备**，对本从站配不上。它的 PDO 清单也不一样（见 §3 末尾）。

---

## 2. 编码器：22 位

**4,194,304 counts/转**（= `1048576 × 4`）。

这个数是**量出来的**：给 1,048,576 counts 的位置指令，实测摆角正好 **±90°**。

**为什么必须先知道它** —— "电机没在转"的误判全出在这里：

| 指令幅值 [counts] | 实际摆角（22 位下） | 肉眼 |
|---|---|---|
| 50,000 | ±4.3° | **几乎看不见** ← 最初的误判 |
| 262,144 | ±22.5° | 勉强 |
| **1,048,576** | **±90°** | 清楚 |

换算是线性的：`摆角(度) = counts ÷ 4194304 × 360`。

---

## 3. 默认 PDO 映射（`ethercat pdos -p0` 实测）

**SM2 / RxPDO `0x1600`（主站 → 从站）**

| 条目 | 位 | 含义 |
|---|---|---|
| `0x6040:00` | 16 | 控制字 |
| `0x607a:00` | 32 | 目标位置 |
| `0x60b8:00` | 16 | 探针功能 |

**SM3 / TxPDO `0x1a00`（从站 → 主站）**

| 条目 | 位 | 含义 |
|---|---|---|
| `0x603f:00` | 16 | **错误码** |
| `0x6041:00` | 16 | 状态字 |
| `0x6061:00` | 8 | 运行模式显示 |
| `0x6064:00` | 32 | 实际位置 |
| `0x60b9:00` | 16 | 探针状态 |
| `0x60ba:00` | 32 | 探针位置 1 |
| `0x60fd:00` | 32 | 数字输入 |

**❌ 不在默认映射里、但经常要用的两个：**

| 对象 | 为什么需要 | 变通办法 |
|---|---|---|
| **`0x6060`** 运行模式设置 | 不设模式就没法进 CSP | **只能走 SDO**（§7） |
| **`0x6077`** 实际力矩 | 想监控力矩 | **拿不到**；要么改 PDO 映射，要么走 SDO 周期读（不现实） |

> 参考实现里注册了 `0x6060` 和 `0x6077` —— **在本从站上这两条注册会失败**。
> 那台设备的默认映射和这台不一样。

---

## 4. SDO 的坑：不支持 SDO Info

`Enable SDO Info: no` 意味着 **IgH 读不到这台驱动器的对象字典**。后果：

```bash
ethercat upload   -p0 0x6060 0x00
# → Failed to determine SDO entry data type. Please specify --type.
#   同时 dmesg 报：EtherCAT ERROR 0-0: SDO 0x6060 does not exist!
```

**那条 dmesg 是"字典读不到"，不是"这个对象不存在"** —— 别被它带偏。

**正确用法：显式给类型**

```bash
ethercat upload   -p0 -t int8   0x6060 0x00      # 读运行模式设置
ethercat upload   -p0 -t int8   0x6061 0x00      # 读运行模式显示
ethercat download -p0 -t int8   0x6060 0x00 8    # 写 CSP
ethercat upload   -p0 -t uint16 0x6041 0x00      # 读状态字
```

---

## 5. 上电时通常已经在故障态

**每次主站起来，驱动器多半已经停在 Fault**（状态字 `0x0618`，bit3=1）。
实测错误码 **`0x8211`**（33297，CiA402 厂商自定义段 `0x8xxx`；具体含义查 L6N 手册）。

**关键在于：一次 Fault reset 就清掉了** —— 说明是**锁存的旧故障**，不是持续存在的工况问题（过流/过压那种会一直触发）。

> **CiA402 的 Fault 必须先复位，否则后面写 Shutdown / Switch On / Enable 全部无效。**
> 我们在这上面卡过一轮：状态机停在 Shutdown 阶段反复超时，状态字一直是 `0x0618`。

---

## 6. 跑通的状态机序列（实测）

```
0x0618  Fault                ──写控制字 0x0080（Fault reset）──▶
0x0631  Ready to switch on   ──写 0x0006（Shutdown）──────────▶
0x0633  Switched on          ──写 0x0007（Switch On）─────────▶
0x0637  Operation enabled    ──写 0x000F（Enable Operation）──▶
0x1237  运行中（位置跟随）
```

**控制字**

| 值 | 含义 |
|---|---|
| `0x0080` | **Fault reset**（bit7） |
| `0x0000` | Disable voltage |
| `0x0006` | Shutdown |
| `0x0007` | Switch On |
| `0x000F` | Enable Operation |
| `0x001F` | Enable Operation + New setpoint（CSP 每周期用这个） |

**状态字判据**（`status & 0x6F`）

| 掩码结果 | 状态 |
|---|---|
| `0x21` | Ready to switch on |
| `0x23` | Switched on |
| `0x27` | Operation enabled |
| bit3 (`0x0008`) 置位 | Fault |

> 运行中的 `0x1237` 解出来：bit0/1/2 全 1（使能链）、bit3=0（无故障）、
> bit4=1（Voltage enabled）、bit5=1、bit9=1（Remote）、bit12=1。

---

## 7. 运行模式：CSP(8)

**必须用 SDO 写 `0x6060:00 = 8`** —— 它不在默认 RxPDO 里，PDO 写不了。

- 写下去后 `0x6061`（模式显示）会读到 8
- **掉电 / 主站重启后保持**（实测：程序跑完后手动重启主站，读回来还是 8）
- 也可以在 `0x6060` 里写别的模式验证读写通，比如 `1`（PP，位置模式）—— 读回也是 1

---

## 8. 已知的坑（按踩到的顺序）

**① 只读不写 → 从站在 OP / PREOP 之间闪断**

只注册 TxPDO 的条目时，SM2 不进域 → 从站收不到有效输出数据 →
AL 状态报 **`0x0018` "No valid inputs available"**，每约 5 秒从 OP 掉回 PREOP。

**加上 SM2 的两条写条目（`0x6040`、`0x607a`）之后**：
| | 只读 | 加了 SM2 |
|---|---|---|
| 域大小 | 19 字节 | **27 字节** |
| 工作计数器 | **0/1** | **3/3** |
| AL 状态 | 闪断 | **稳定 OP** |
| dmesg | 反复 `0x0018` | 干净 |

**② 指令幅值太小 → 误判"电机没在转"**

见 §2。50000 counts 只有 ±4.3°，看不出来。

**③ 1 Hz 打印正好落在正弦零点**

0.5 Hz 正弦（周期 2 s）用 1 Hz 采样，拿到的永远是 `±A·sin(φ)` 这一个交替序列 ——
**分不出"真在摆动"和"根本没动"**。打印要 ≥10 Hz（我们用的 100 ms，每周期 20 个点）。

**④ Fault 没复位 → 状态机走不动**

见 §5。

**⑤ 退出时直接 release master → 驱动器停在使能态**

程序退出前必须先把控制字打回 `0x0006`（Shutdown）或 `0x0000`（Disable voltage），
**并且再跑几拍**让它真的发下去，然后才 release master。
我们的 `ethercat_south_l6n_control.c` 里有 `ethercat_south_l6n_shutdown()`，
但 collector 的死循环收不到 SIGTERM，所以目前**没人调得到它** —— 这个缺口还在。

---

## 9. 实测数据汇总

| 项 | 值 |
|---|---|
| 域大小 | **27 字节**（TxPDO 19 + RxPDO 8） |
| 期望工作计数器 | **3** |
| 启动瞬态 | 前 ~105 拍 WC 异常（从站还没进 OP），之后全正常 |
| 跟随性能 | 0.5 Hz / ±1048576 counts 时，峰值误差 **1120 counts（0.11%）** |
| 1 kHz 触发 | 实测每拍 1000 次/秒，稳定 |
| 主站 Idle 时 | 从站停 **PREOP**（正常） |
| 主站停止后 | 从站回 PREOP |

---

## 10. 相关文件

| 文件 | 说明 |
|---|---|
| `test-file/ethercat_south_l6n_control.{c,h}` | 驱动本从站的代码（CiA402 状态机 + 故障复位 + CSP 正弦指令） |
| `test-file/ethercat_south_connect.c` | 配置期：建域、注册 PDO、SDO 设模式 |
| `test-file/ethercat_south_collector.c` | 周期期：每拍调状态机 |
| `../连接指南.md` | 怎么连上那台 openEuler / SylixOS 工控机 |
| `aiworkplace/SUHUAN/ethercat_l6n_working.c` | 参考实现，**但针对另一台设备**（vendor/product 和 PDO 清单都不同） |
