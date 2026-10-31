# ch32v003-keyboard（开源包 · 自包含）

CH32V003F4P6：4×4 矩阵扫键 + EC11 + 充电检测，I2C 从机上报 ESP32-S3。

本目录已内嵌官方 EVT 所需子集（`Core` / `Peripheral` / `Startup` / `Debug` / `Ld`），**无需再链外部 `CH32V003EVT`**。用 MounRiver 打开本文件夹即可编译。

协议：`User/kb_regs.h`（WHOAMI=`0xCB`，VERSION 以头文件为准）。

## 仅烧录（不开发）

1. 安装 [MounRiver Studio](http://www.mounriver.com/) 或 WCH 官方烧录工具  
2. WCH-Link 连接：`SWIO` + `GND` + `3V3`（与板共地）  
3. 烧写预编译固件：`prebuilt/ch32v003-keyboard.hex`

## 二次开发

1. MounRiver Studio → Open Project → 选择本目录  
2. 确认工程内能看到 `User` / `Core` / `Peripheral` / `Startup` / `Debug` / `Ld`  
3. Build → Download  

## 引脚（HW V2）

| 功能 | 脚 |
|------|-----|
| I2C SDA/SCL | PC1 / PC2 |
| INT# | PC0 |
| row1–4 | PC3 / PC4 / PC5 / PD6 |
| col1–4 | PC6 / PC7 / PD2 / PD5 |
| ENC A/B | PD4 / PD3 |
| XSMT | PD0 |
| STDBY / CHARG | PA1 / PA2 |

I2C 7-bit 地址：**0x28**。



 开源仓库：[TheHotSummery/sky-music-ljc-opensource](https://github.com/TheHotSummery/sky-music-ljc-opensource)
