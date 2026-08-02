# MoveToPlay Companion Prototype 01

这是 MoveToPlay 的 Windows 桌面端悬浮层原型。当前版本使用模拟运动数据，重点验证：

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

## 运行

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj
```

或者构建后运行：

```powershell
dotnet build companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -c Release
companion/MoveToPlay.Companion/bin/Release/net8.0-windows/MoveToPlay.Companion.exe
```

## 当前边界

- 数据源目前是 `DemoTelemetryService`，尚未连接 Dongle USB CDC。
- 心率为演示值；真实心率需要 BLE 心率设备或后续 PPG 传感器。
- 估算卡路里将来根据用户体重、动作 MET 和心率修正。
- WPF 置顶层适用于窗口化和无边框全屏。真正的独占全屏可能覆盖普通桌面窗口。
- 悬浮层不注入游戏进程，只创建独立透明窗口。

## 后续接入点

真实数据源只需实现 `Services/ITelemetrySource.cs`。Dongle 完成 HID + CDC 后，可增加 `UsbCdcTelemetryService` 替换演示数据源，界面和悬浮层无需重写。

主题位于 `Profiles/*.json`，后续为不同游戏增加 JSON 配置即可。自动绑定支持精确进程名，也支持“进程名 + 窗口标题”组合规则，避免误认通用的 Unreal/Java 进程。
