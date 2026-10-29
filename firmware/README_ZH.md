# 出厂固件

[English](README.md) | [简体中文](README_ZH.md)

适用于 16 MB Flash 的 ESP32-C6-Touch-LCD-2.8。每个文件都是从 `0x0` 烧录的合并镜像，文件名中的日期表示版本；最新版本在上，旧版本保留。

| 文件 | 用途 |
| --- | --- |
| [ESP32-C6-Touch-LCD-2.8_FactoryOnly_20261029.bin](ESP32-C6-Touch-LCD-2.8_FactoryOnly_20261029.bin) | 当前出厂桌面：包含语音对话、天气、SD 音乐及硬件测试等 11 个应用；仅支持 2.4 GHz Wi-Fi。 |
| [ESP32-C6-Touch-LCD-2.8_FactoryOnly_20260711.bin](ESP32-C6-Touch-LCD-2.8_FactoryOnly_20260711.bin) | 原版出厂演示。 |

下载上面的 BIN，在文件所在文件夹运行以下命令，将 `<PORT>` 替换为开发板的串口。擦除 Flash 会清除已保存的 Wi-Fi、激活信息和设置。

```bash
python -m pip install esptool
python -m esptool --chip esp32c6 --port <PORT> erase_flash
python -m esptool --chip esp32c6 --port <PORT> write_flash 0x0 ESP32-C6-Touch-LCD-2.8_FactoryOnly_20261029.bin
```

烧录完成后按 RESET，在设置中连接 2.4 GHz 网络，再按照[应用使用说明](../examples/factory/USER_GUIDE_ZH.md)操作。出厂源码位于 [`examples/factory`](../examples/factory/README_ZH.md)。
