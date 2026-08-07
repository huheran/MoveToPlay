# MoveToPlay Companion Prototype 01

这是 MoveToPlay 的 Windows 桌面端运动伴侣。当前版本默认通过 Dongle 的 USB CDC 复合接口接收真实动作数据，同时保留显式模拟模式用于没有硬件时预览界面。

- 透明置顶、鼠标点击穿透的游戏悬浮层；
- 自动跟随目标游戏窗口；
- 切出游戏时自动隐藏；
- 悬浮层只绘制文字、状态点和目标进度线，不显示任何卡片底色或边框；
- 文字使用字形级黑色描边与零偏移柔和阴影，在明暗复杂的游戏画面上仍可辨认；
- 8 套配置化主题：原神、鸣潮、我的世界、艾尔登法环、GTA V、赛博朋克 2077、通用和科幻竞技；
- 每套主题可独立配置文字、标记符号、字体、进度线样式、悬浮位置和运动激励；
- 当前动作、心率、估算卡路里、运动时间和连击展示；
- 游戏化运动激励动画；
- `Ctrl + Shift + F10` 全局显示/隐藏快捷键。
- 点击主窗口右上角关闭按钮后隐藏到 Windows 系统托盘，动作接收和统计继续运行；双击托盘图标恢复，右键菜单可真正退出。
- 每个游戏主题可以独立选择自动检测或常见 720p、1080p、2K、4K、16:10、超宽屏分辨率，并分别设置悬浮层的九宫格锚点和水平/垂直偏移；设置自动保存到 `%LOCALAPPDATA%\MoveToPlay\overlay-placements.json`，切换主题或分辨率时自动恢复。

## 运行

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj
```

或者构建后运行：

```powershell
dotnet build companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -c Release
companion/MoveToPlay.Companion/bin/Release/net8.0-windows/MoveToPlay.Companion.exe
```

软件默认自动查找 USB VID `303A`、PID `4005` 的 MoveToPlay Dongle，并在设备插拔后自动重连。也可以手动指定端口和用户体重：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -- --port=COM35 --weight=68
```

只有需要预览随机模拟数据时才使用：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -- --demo
```

## Dongle 遥测协议

Dongle 在 Play 模式下枚举为 HID 键盘/鼠标与 CDC 虚拟串口复合设备。CDC 每 100 ms 发送一行 UTF-8 JSON，电脑端以换行符分包。当前协议版本为 1，内容包括：

- Dongle 序号与单调时间戳；
- 当前动作、识别置信度、运动强度和有效运动状态；
- 实际键鼠动作事件与累计事件数；
- 四个 Tracker 的在线掩码、在线数量和信号质量；
- Blade 在线状态及各设备电量。

电脑端根据动作 MET、运动强度、有效运动时间和用户体重估算卡路里。默认体重暂为 68 kg，可用 `--weight` 修改。识别置信度只表示模型对动作类别的确定程度，不作为运动强度使用。

## 当前边界

- 默认数据源是 `UsbCdcTelemetryService`；`DemoTelemetryService` 只在 `--demo` 模式启用。
- 心率不在运动过程中传输，界面预留为运动结束后静止测量。
- 当前卡路里是基于动作 MET、Dongle 运动强度、有效运动时间和体重的估算值，不用于医疗判断。
- WPF 置顶层适用于窗口化和无边框全屏。真正的独占全屏可能覆盖普通桌面窗口。
- 悬浮层不注入游戏进程，只创建独立透明窗口。

## 后续接入点

数据源统一实现 `Services/ITelemetrySource.cs`。真实串口由 `UsbCdcTelemetryService` 负责自动发现、解析和重连，模拟预览由 `DemoTelemetryService` 提供。

主题位于 `Profiles/*.json`，后续为不同游戏增加 JSON 配置即可。自动绑定支持精确进程名，也支持“进程名 + 窗口标题”组合规则，避免误认通用的 Unreal/Java 进程。
