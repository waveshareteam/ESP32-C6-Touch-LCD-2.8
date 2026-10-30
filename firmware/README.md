# Factory Firmware

[English](README.md) | [简体中文](README_ZH.md)

Ready-to-flash firmware for ESP32-C6-Touch-LCD-2.8 with 16 MB Flash. The file is a merged image flashed at `0x0`; the date in its file name is its version.

| File | Purpose |
| --- | --- |
| [ESP32-C6-Touch-LCD-2.8_FactoryOnly_20261029.bin](ESP32-C6-Touch-LCD-2.8_FactoryOnly_20261029.bin) | Current factory desktop: 11 apps including voice chat, weather, SD music and hardware tests; 2.4 GHz Wi-Fi only. |

Download the BIN above, then run the following commands from its folder. Replace `<PORT>` with your board's serial port. Erasing Flash removes saved Wi-Fi credentials, activation data and settings.

```bash
python -m pip install esptool
python -m esptool --chip esp32c6 --port <PORT> erase_flash
python -m esptool --chip esp32c6 --port <PORT> write_flash 0x0 ESP32-C6-Touch-LCD-2.8_FactoryOnly_20261029.bin
```

Press RESET after flashing. Open Settings to connect a 2.4 GHz network, then follow the [application guide](../examples/factory/USER_GUIDE.md). Factory source is in [`examples/factory`](../examples/factory/).
