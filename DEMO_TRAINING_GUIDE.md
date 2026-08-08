# MoveToPlay 决赛训练闭环操作说明

更新时间：2026-08-08

## 已经准备好的部分

- Windows Companion 可完成采集标注、SSH 连接、可续传上传、任务等待、指标展示、产物下载和人工批准。
- 阿里云 API 只监听 `127.0.0.1:8000`，电脑端通过 SSH 隧道访问，不需要域名和备案。
- `build-dongle-collect/` 已生成 16 MB Dongle Data Collect 固件。
- 本机已缓存训练 Job `9968a258eb204188b18682471f30a4d8` 的清单和关键模型 C 文件，断网时仍能显示真实指标。
- 云端已有完整双随机森林训练结果，但保持未批准，人工批准必须由演示者确认指标后点击。

## 一、烧录数据采集 Dongle

先连接 Dongle，确认设备管理器中的 COM 口，然后在 ESP-IDF PowerShell 环境执行：

```powershell
idf.py -B build-dongle-collect -p COMx flash monitor
```

这个构建目录已经固定使用 `M2P_BOARD_PROFILE=1` 和 `M2P_DONGLE_MODE=1`。不要把 Data Collect 固件当作正式游戏固件；它输出原始 CSV，不启用 HID。

## 二、采集并标注

1. 启动 `MoveToPlay.Companion.exe`。
2. 点击主窗口左侧“数据采集与云端训练”。
3. 在“采集与标注”页刷新并选择 Dongle COM 口。
4. 打开四个 Tracker，确认“在线节点”逐步出现四个编号。
5. 选择连续状态标签，点击“开始采集”。
6. 事件动作做到最明显的位置时点击一次对应事件按钮。
7. 每种动作分多轮录制；动作前后各留约两秒，切换标签后先停一秒。
8. 点击“停止并保存”，软件会把本次两张 CSV 自动带到云端页。

采集原始文件默认在：

```text
文档\MoveToPlay\collections\session-年月日-时分秒\
```

## 三、上传和训练

1. 切换到“云端训练”。
2. 点击“连接阿里云服务器”，状态应变为“阿里云已安全连接”。
3. 检查 `samples.csv`、`events.csv` 和数据集名称。
4. 点击“上传并开始训练”。
5. 等待状态依次变化：登记数据集、上传、入队、运行、通过。
6. 查看状态随机森林和事件随机森林的准确率、Macro F1、质量门禁。
7. 先点击“下载全部产物”留存一份，再决定是否“人工批准此模型”。

上传中断后不要修改 CSV，重新点击会查询服务器接收偏移并继续。点击“停止等待”只停止电脑端轮询，不会删除服务器任务。也可以输入 32 位 Job ID 加载已经存在的任务。

## 四、模型进入固件

云端产物的 `generated/` 目录包含两个模型的 C 数组。批准只记录人工决策，不会悄悄改写 Dongle：

```text
generated/rf_state_model_generated.c
generated/rf_state_model_generated.h
generated/rf_model_generated.c
generated/rf_model_generated.h
```

正式替换 `main/generated/` 前必须确认：

- 两个模型质量门禁通过；
- 类别顺序和特征数量符合固件；
- 下载文件哈希与 `run_manifest.json` 一致；
- 演示者已经点击人工批准；
- 替换后重新构建 Play 固件并做实机动作/HID 回归。

## 五、现场兜底

- 首选比赛场地 Wi-Fi；不稳定时切到手机热点，SSH 不要求固定的电脑公网地址。
- 服务器不可达时，云端页会显示本机缓存的上次真实训练指标与关键 C 文件。
- 保留一份已经验证过的 Play 固件，不要在正式展示前只留下新训练但尚未回归的固件。
- 现场不要开放 ECS 的 8000 端口，也不要展示 API 令牌。

## 用户必须参与的最后验收

当前电脑没有识别到 Dongle，只看到系统/蓝牙串口，因此以下步骤必须在硬件接入后由用户配合：

1. 插入 Dongle，必要时按 BOOT/RESET 完成烧录。
2. 开启并佩戴四个 Tracker 与 Blade，执行真实动作。
3. 确认采集节点、动作标记时机和样本质量。
4. 查看最终指标并亲自决定是否批准。
5. 烧录 Play 固件后确认游戏键鼠行为符合预期。
