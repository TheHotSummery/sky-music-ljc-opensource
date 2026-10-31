# ESP32-S3 源码（二次开发）

本目录为开源快照：**不含** `build/` 与 `managed_components/`（体积大，由 IDF 组件管理器拉取）。

## 环境

- ESP-IDF **5.5.x**（与开发时一致）
- 目标芯片：ESP32-S3，Flash 建议 **16MB**（N16R8）

```powershell
. <IDF>/export.ps1
cd <this-folder>
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash          # 不含 assets
idf.py -p COMx assets-flash   # 出厂资源（改过 assets/factory 后）
```

首次 `build` 会下载 `managed_components`（音频编解码等）。

出厂资源源文件：`assets/factory/`（也在开源包根目录 `assets/factory/` 有一份副本）。

预编译二进制见 `../flash/`。
