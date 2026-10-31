# CH32 固件开发规划（先核对，再改代码）

**流程约定：** 你先按本文核对引脚与二极管方向 → 回复「引脚 OK」→ 我们再在 `ch32v003-keyboard` 上改/补固件 → 通过后再做 ESP32 `key_link`。

工程：`sky-music-v1/ch32v003-keyboard/`  
协议头文件（源码权威）：`ch32v003-keyboard/User/kb_regs.h`  
本文是给人核对的定稿说明；与头文件冲突时以头文件 + 实板为准。

---

## 0. CH32 在系统里干什么

| 做 | 不做 |
|----|------|
| 4×4 矩阵扫键（K1–K15 + 编码器键） | 不驱动 WS2812 |
| EC11 正交计数（主旋钮） | 不管 PCM5102 / 屏 / TF |
| I2C **从机** 0x28 给 ESP32 | 不升 5V |
| 有事件拉低 **INT#** | |
| （规划）读充电状态脚 CHARG/STDBY | 电量计 CW2015 由 S3 直接读 |

---

## 1. 全引脚连接表（请逐项核对）

封装：**CH32V003F4P6 TSSOP-20**（U2）

| 脚号 | 端口 | 原理图网络 | 接到哪里 | 固件用途 | 核对 |
|------|------|------------|----------|----------|------|
| 1 | PD4 | `TIM2_CH1` | EC11 U3 相位 A（上拉+滤波在板上） | TIM2 编码器 CH1 | ☐ |
| 2 | PD5 | `col4` | 矩阵列 4（经二极管） | 列输入上拉 | ☐ |
| 3 | PD6 | `row4` | 矩阵行 4 | 行推挽输出 | ☐ |
| 4 | PD7/NRST | `NRST` | **仅** C1 100n→GND（作复位，不当 GPIO） | 复位 | ☐ |
| 5 | PA1 | `STDBY` | TP4056 充满状态分压 | **规划**：输入读充满 | ☐ |
| 6 | PA2 | `CHARG` | TP4056 充电中分压 | **规划**：输入读充电中 | ☐ |
| 7 | VSS | `GND` | 地 | 电源 | ☐ |
| 8 | PD0 | — | 原理图未接网标 | NC | ☐ |
| 9 | VDD | `VCC_3V3` | 3V3 + C50 | 电源 | ☐ |
| 10 | PC0 | `INT#` | → ESP32 GPIO47（须上拉） | 有事件拉低 | ☐ |
| 11 | PC1 | `I2C1_SDA` | → ESP32 GPIO40 + 4.7k | I2C 从机 SDA | ☐ |
| 12 | PC2 | `I2C1_SCL` | → ESP32 GPIO41 + 4.7k | I2C 从机 SCL | ☐ |
| 13 | PC3 | `row1` | 矩阵行 1 | 行输出 | ☐ |
| 14 | PC4 | `row2` | 矩阵行 2 | 行输出 | ☐ |
| 15 | PC5 | `row3` | 矩阵行 3 | 行输出 | ☐ |
| 16 | PC6 | `col1` | 矩阵列 1 | 列输入 | ☐ |
| 17 | PC7 | `col2` | 矩阵列 2 | 列输入 | ☐ |
| 18 | PD1 | `SWIO` | H1 下载口 | WCH-Link 单线 | ☐ |
| 19 | PD2 | `col3` | 矩阵列 3 | 列输入 | ☐ |
| 20 | PD3 | `TIM2_CH2` | EC11 U3 相位 B | TIM2 编码器 CH2 | ☐ |

> 按 PDF 网标坐标：**PA1=`STDBY`，PA2=`CHARG`，脚4=`NRST`**（不要把 PD7 当充电 GPIO）。若你板子丝印不同，以实板为准并改表。

### 1.1 矩阵键位

| | col1 (PC6) | col2 (PC7) | col3 (PD2) | col4 (PD5) |
|---|------------|------------|------------|------------|
| **row1 (PC3)** | K1 | K2 | K3 | K4 |
| **row2 (PC4)** | K5 | K6 | K7 | K8 |
| **row3 (PC5)** | K9 | K10 | K11 | K12 |
| **row4 (PD6)** | K13 | K14 | K15 | **KEY16 = EC11 开关** |

`key_id`：K1→0 … K15→14，编码器键→**15**。

### 1.2 扫键极性（与现有 `kb_hw.c` 一致，必须和二极管方向匹配）

```text
行：空闲输出高，扫描时该行拉低
列：内部上拉输入；键按下且二极管导通 → 列被拉低 → 读到 0 表示按下
```

请核对二极管：**按下时能否把「被拉低的行」耦合到「列」使列变低**。  
若方向反了，固件要把行/列驱动逻辑对调（改 `kb_hw.c`，协议不变）。

### 1.3 EC11（键盘侧 U3）

| EC11 | 网络 | CH32 |
|------|------|------|
| A | `TIM2_CH1` | PD4 |
| B | `TIM2_CH2` | PD3 |
| C | GND | — |
| SW | `KEY16_A/B` → 进矩阵 row4×col4 | 经 D16 |

板上应有 10k 上拉 + 100n 滤波（A/B）。

---

## 2. I2C 协议定稿（v1）

### 2.1 物理层

| 项 | 值 |
|----|-----|
| 角色 | CH32 = **Slave**，ESP32 = Master |
| 7-bit 地址 | **0x28**（写地址字节 0x50，读 0x51） |
| 引脚 | SDA=PC1，SCL=PC2（与 EVT 例程默认一致） |
| 上拉 | 板级 4.7k→3V3（在 ESP 侧附近） |
| 速率 | 先 **100 kHz**，稳定后再试 400 kHz |
| INT# | PC0，空闲高，有 pending **拉低**；S3 GPIO47 下降沿/低电平查询 |

### 2.2 事务格式

```text
写寄存器指针:  START + ADDR_W + [reg] + STOP
读:            START + ADDR_W + [reg] + RESTART + ADDR_R + DATA… + STOP

读时：除 EVT_DATA(0x07) 外，每读 1 字节寄存器指针自动 +1
EVT_DATA：指针固定 0x07，便于连读弹出 FIFO
```

### 2.3 寄存器表

| 偏移 | 名称 | R/W | 说明 |
|------|------|-----|------|
| 0x00 | WHOAMI | R | 固定 **0xCB**（连通性探针） |
| 0x01 | VERSION | R | 协议版本，现 **0x01**；改协议则 +1 |
| 0x02 | STATUS | R | bit0=`KEY` 键位相对上次主机侧有变化标志（见实现） / bit1=`ENC` 有未读增量 / bit2=`EVT` FIFO 非空 |
| 0x03 | KEYMASK_L | R | bit0..7 = K1..K8（1=按下） |
| 0x04 | KEYMASK_H | R | bit0..6 = K9..K15；**bit7 = 编码器键 KEY16** |
| 0x05 | ENC_DELTA | R | **int8**，顺时针为正（约定）；**读清零** |
| 0x06 | EVT_COUNT | R | FIFO 中事件条数（每条= key_id + state 两字节） |
| 0x07 | EVT_DATA | R | 弹出：第 1 字节 `key_id`，第 2 字节 `state`(0松/1按下)；空则读 0xFF |
| 0x08 | CHG_RAW | R | bit0=CHARG(PA2) bit1=STDBY(PA1)，1=高 |
| 0x09 | CHG_FLAGS | R | bit0充电中 / bit1充满 / bit2似有VBUS |

当前源码已实现 **0x00–0x09**，`VERSION=0x02`。

### 2.4 STATUS / INT 行为

| 条件 | STATUS | INT# |
|------|--------|------|
| 有未弹出的按键边沿 | EVT=1 | 低 |
| ENC_DELTA ≠ 0 | ENC=1 | 低 |
| KEY 标志（实现相关） | KEY=1 | 低 |
| 全部清完 | 0 | 释放为高 |

**主机推荐读法（INT 下降沿或轮询）：**

```text
1. 写指针 0x02，连读 4 字节: STATUS, KEYMASK_L, KEYMASK_H, ENC_DELTA
   （ENC_DELTA 读清）
2. 若 STATUS.EVT：写指针 0x07，每次读 2 字节直到 EVT_COUNT==0
3. INT 应变高；若仍低，再读一遍 STATUS
```

### 2.5 编码器刻度

现实现：TIM2 四倍频计数，**约 4 个正交计数 = 1 格** 累加进 `ENC_DELTA`。  
实机若一格跳多/跳少，只改 `kb_hw.c` 分频，协议不变。

---

## 3. 现有工程状态

| 文件 | 内容 |
|------|------|
| `User/kb_regs.h` | 寄存器与地址（**v2**） |
| `User/kb_hw.c/h` | 矩阵、TIM2 编码器、INT、充电脚 |
| `User/kb_app.c/h` | I2C 从机 + FIFO + 寄存器 |
| `User/main.c` | 1 ms 主循环 |

**已完成（2026-10-02）：** 充电脚 PA1/PA2 + 寄存器 0x08/0x09。

**上板冒烟：**

1. MounRiver 编译下载  
2. 读 WHOAMI=`0xCB`、VERSION=`0x02`  
3. 按键 / 旋钮 → INT 拉低 → 读 KEYMASK / ENC / EVT  
4. 插 USB 充电时看 CHG_FLAGS

---

## 4. 开发步骤

```text
Phase A  你核对 §1 引脚 + 二极管 + INT 上拉
Phase B  编译烧录 CH32 → 主机/逻辑仪读 WHOAMI=0xCB
Phase C  按键/旋钮 → INT 抖动 → 读 KEYMASK / ENC_DELTA / EVT
Phase D  （可选）加充电寄存器
Phase E  ESP32 key_link 改 I2C 主机（另开）
```

---


