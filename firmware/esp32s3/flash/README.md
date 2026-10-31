# ESP32-S3 预编译固件（N16R8 / 16MB Flash）

无需安装 ESP-IDF 即可烧录。需本机 Python + [`esptool`](https://pypi.org/project/esptool/)：

```powershell
pip install esptool
```

## 文件

| 文件 | 偏移 | 说明 |
|------|------|------|
| `bootloader.bin` | `0x0` | 引导 |
| `partition-table.bin` | `0x8000` | 分区表 |
| `esp32s3-music-keyboard.bin` | `0x10000` | 应用固件 |
| `assets.bin` | `0x310000` | 出厂资源（钢琴/背景/演示谱） |
| `partitions.csv` | — | 分区说明（对照用） |

## 烧录

空片请先进下载模式：**按住 BOOT → 复位/上电 → 松开 BOOT**。

```powershell
# 一次性全烧（推荐首烧）
.\flash_all.ps1 COM5

# 或分开：
.\flash_app.ps1 COM5
.\flash_assets.ps1 COM5
```

等价手动命令见各 `.ps1`。量产 **N8** 板请改用对应分区偏移，勿直接用本包 `0x310000`。

## 二次开发

见上级目录 `../source/`（需自行安装 ESP-IDF 5.5.x）。

开源仓库：[TheHotSummery/sky-music-ljc-opensource](https://github.com/TheHotSummery/sky-music-ljc-opensource)
