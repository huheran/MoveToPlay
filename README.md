# ESP-IDF 基础模板

这是一个面向后续项目复用的最小 ESP-IDF 工程模板。

特点：

- 保留最小可运行的 GPIO LED 闪烁验证
- 保留 ESP-IDF 必要构建骨架
- 去掉官方示例中的演示性依赖和冗余文件
- 适合作为传感器、通信、任务调度等功能的起点

## 目录结构

```text
esp-idf-template/
├── CMakeLists.txt
├── README.md
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    └── app_main.c
```

## 快速使用

1. 进入工程目录
2. 运行 `idf.py build`
3. 运行 `idf.py -p 端口 flash monitor`

如果板载 LED 不在 `GPIO38`，修改 [main/app_main.c](/mnt/d/MyProject/Embedded-Design/esp-idf-template/main/app_main.c) 顶部的 `BLINK_GPIO` 即可。

## Dongle / Tracker 构建配置

dongle 是 16MB flash，tracker / blade 是 8MB flash。默认配置统一按 8MB 构建，16MB dongle 可以兼容 8MB 分区，只是后半段 flash 暂时不用。

默认构建/烧录命令：

```powershell
idf.py build
idf.py -p COMx flash monitor
```

如果想用独立 build 目录构建 8MB 固件：

```powershell
idf.py -B build-tracker8mb -D "SDKCONFIG=build-tracker8mb/sdkconfig" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.8mb" build
idf.py -B build-tracker8mb -p COMx flash monitor
```

如果后续 dongle 固件确实需要使用 16MB 大分区，再用显式 16MB 配置：

```powershell
idf.py -B build-dongle16mb -D "SDKCONFIG=build-dongle16mb/sdkconfig" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.16mb" build
idf.py -B build-dongle16mb -p COMx flash monitor
```

使用 VS Code 的 ESP-IDF 按钮编译和烧录前，直接修改 `main/app_main.c` 顶部的 `M2P_BOARD_PROFILE`：`1`=Dongle、`2`=Blade、`3`=胸部、`4`=右手、`5`=左手、`6`=腿部。Dongle 只有一份统一固件，长按 GPIO4 两秒在 Play、数据采集和 Wi-Fi 维护三个运行状态间循环。

橙色采集态会给每条 Tracker 数据附加 Dongle 接收时间，并把 Blade 的“松开→按下”边沿作为独立事件发送到同一个 CDC 串口。Companion 支持预选或新增动作事件、Blade 即时标记、倒计时提示音和延迟补偿；同一次长按只记录一个事件。

## Dongle Wi-Fi 配置页

Dongle 橙灯采集态下再长按 GPIO4 两秒进入蓝灯 Wi-Fi display mode；连接 `MoveToPlay-Dongle`，打开 `http://192.168.4.1/`。

页面上方会实时显示 tracker 在线状态和电量；页面下方的 Controls 表格可以修改每个识别动作对应的输出、按键、修饰键、触发方式、置信度阈值、冷却时间和持续帧数。保存后配置会写入 dongle NVS，重启或退出 Wi-Fi display mode 后继续生效。恢复默认值可以点 `Reset defaults`。

键鼠配置按模型中的动作英文 ID 保存，而不是按类别序号保存。新模型增加动作后网页会自动增加一行，新动作默认禁用输出；用户设置后不会因为下次模型类别排序变化而错配到其他动作。旧版序号配置首次启动时会自动迁移。

## 模板原则

- 先保证最小跑通
- 所有扩展功能后加
- 默认只保留长期有价值的工程骨架
噜噜噜噜啦啦啦啦啦啦啦的小修改尝试git
＋1噜噜噜噜啦啦啦啦啦
