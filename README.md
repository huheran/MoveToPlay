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

## 模板原则

- 先保证最小跑通
- 所有扩展功能后加
- 默认只保留长期有价值的工程骨架
