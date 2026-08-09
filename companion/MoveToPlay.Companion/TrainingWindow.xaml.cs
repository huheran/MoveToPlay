using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Win32;
using MoveToPlay.Companion.Models;
using MoveToPlay.Companion.Services;
using Forms = System.Windows.Forms;

namespace MoveToPlay.Companion;

public partial class TrainingWindow : Window
{
    private static readonly TrainingLabelOption[] StateLabels =
    [
        new("idle", "待机 idle"),
        new("walk", "行走 walk"),
        new("run", "奔跑 run"),
        new("move_noise", "过渡 / 干扰 move_noise"),
        new("right_hand_slash", "右手挥砍状态 right_hand_slash"),
    ];

    private static readonly BladeMarkingModeOption[] BladeMarkingModes =
    [
        new(BladeMarkingMode.Immediate, "即时标记（应用延迟补偿）"),
        new(BladeMarkingMode.Countdown, "倒计时提示音（推荐）"),
    ];

    private readonly Action _pauseTelemetry;
    private readonly Action _resumeTelemetry;
    private readonly Func<TelemetrySnapshot?>? _getTelemetrySnapshot;
    private readonly Func<Task>? _refreshTelemetry;
    private readonly ImuCollectionService _collector = new();
    private readonly EventCatalogService _eventCatalog = new();
    private readonly ObservableCollection<TrainingEventOption> _eventOptions = [];
    private readonly ObservableCollection<LocalCollectionSession> _collectionSessions = [];
    private readonly ObservableCollection<CloudJob> _jobHistory = [];
    private readonly ObservableCollection<CloudJob> _modelVersions = [];
    private readonly TrainingHistoryService _history = new();
    private readonly CollectionLibraryService _collectionLibrary;
    private readonly FirmwareDeploymentService _firmwareDeployment = new();
    private readonly System.Windows.Threading.DispatcherTimer _deviceStatusTimer = new()
    {
        Interval = TimeSpan.FromMilliseconds(500),
    };
    private SshTunnelService? _tunnel;
    private CloudTrainingApiClient? _api;
    private CancellationTokenSource? _operationCancellation;
    private CloudDataset? _activeDataset;
    private CloudJob? _activeJob;
    private CloudArtifact[] _artifacts = [];
    private FirmwareBuildPackage? _firmwarePackage;
    private CancellationTokenSource? _firmwareCancellation;
    private CollectionCountdownWindow? _countdownWindow;
    private TrainingEventOption? _editingEvent;
    private PreparedCollectionDataset? _preparedDataset;
    private bool _creatingEvent;
    private bool _telemetryPaused;

    public TrainingWindow(
        Action pauseTelemetry,
        Action resumeTelemetry,
        Func<TelemetrySnapshot?>? getTelemetrySnapshot = null,
        Func<Task>? refreshTelemetry = null)
    {
        _pauseTelemetry = pauseTelemetry;
        _resumeTelemetry = resumeTelemetry;
        _getTelemetrySnapshot = getTelemetrySnapshot;
        _refreshTelemetry = refreshTelemetry;
        _collectionLibrary = new CollectionLibraryService(_eventCatalog);
        InitializeComponent();
        StateLabelSelector.ItemsSource = StateLabels;
        StateLabelSelector.SelectedIndex = 0;
        foreach (var option in _eventCatalog.Load())
        {
            _eventOptions.Add(option);
        }
        EventSelector.ItemsSource = _eventOptions;
        EventSelector.SelectedIndex = _eventOptions.Count > 0 ? 0 : -1;
        BladeModeSelector.ItemsSource = BladeMarkingModes;
        BladeModeSelector.SelectedIndex = 0;
        if (Environment.GetCommandLineArgs().Any(argument =>
                argument.Equals("--training-tab=data", StringComparison.OrdinalIgnoreCase)))
        {
            TrainingTabs.SelectedIndex = 1;
        }
        else if (Environment.GetCommandLineArgs().Any(argument =>
                     argument.Equals("--training-tab=cloud", StringComparison.OrdinalIgnoreCase)))
        {
            TrainingTabs.SelectedIndex = 2;
        }
        CollectionRootText.Text = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "MoveToPlay", "collections");
        DatasetNameText.Text = $"决赛演示采集-{DateTime.Now:yyyy-MM-dd-HHmm}";
        CollectionSessionList.ItemsSource = _collectionSessions;
        JobHistoryList.ItemsSource = _jobHistory;
        ModelVersionList.ItemsSource = _modelVersions;
        UpdateCollectorBladeSettings();
        _collector.StatusChanged += Collector_StatusChanged;
        _collector.CountdownChanged += Collector_CountdownChanged;
        _collector.AutomaticSequenceCompleted += Collector_AutomaticSequenceCompleted;
        _deviceStatusTimer.Tick += (_, _) => UpdatePrecollectionDeviceStatus();
        Loaded += (_, _) =>
        {
            RefreshPorts();
            RefreshFirmwarePorts();
            RefreshRollbackPorts();
            RefreshCollectionLibrary();
            LoadCachedResult();
            _deviceStatusTimer.Start();
            UpdatePrecollectionDeviceStatus();
        };
        Closed += TrainingWindow_Closed;
    }

    private void RefreshPorts_Click(object sender, RoutedEventArgs e) => RefreshPorts();

    private void RefreshPorts()
    {
        var previous = PortSelector.SelectedItem as string;
        var ports = ImuCollectionService.GetPortNames();
        PortSelector.ItemsSource = ports;
        PortSelector.SelectedItem = ports.FirstOrDefault(port => port.Equals(previous, StringComparison.OrdinalIgnoreCase));
        if (PortSelector.SelectedIndex < 0 && ports.Length > 0)
        {
            PortSelector.SelectedIndex = ports.Length - 1;
        }
        CollectionStatusText.Text = ports.Length == 0 ? "未发现串口，请连接 Dongle 后刷新" : $"发现 {ports.Length} 个串口，请选择 Dongle";
        UpdateCollectionStartEnabled();
    }

    private void CollectionSelection_Changed(object sender, SelectionChangedEventArgs e) =>
        UpdateCollectionStartEnabled();

    private void ChooseCollectionRoot_Click(object sender, RoutedEventArgs e)
    {
        using var dialog = new Forms.FolderBrowserDialog
        {
            Description = "选择 MoveToPlay 采集会话保存目录",
            SelectedPath = Directory.Exists(CollectionRootText.Text) ? CollectionRootText.Text : "",
            UseDescriptionForTitle = true,
        };
        if (dialog.ShowDialog() == Forms.DialogResult.OK)
        {
            CollectionRootText.Text = dialog.SelectedPath;
            RefreshCollectionLibrary();
        }
    }

    private void StateLabelSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (StateLabelSelector.SelectedItem is TrainingLabelOption label)
        {
            _collector.SetStateLabel(label.Id);
        }
        UpdateCollectionStartEnabled();
    }

    private async void StartCollection_Click(object sender, RoutedEventArgs e)
    {
        if (PortSelector.SelectedItem is not string port)
        {
            MessageBox.Show(this, "请先选择 Dongle 串口。", "无法开始采集", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (StateLabelSelector.SelectedItem is not TrainingLabelOption ||
            EventSelector.SelectedItem is not TrainingEventOption)
        {
            MessageBox.Show(this, "请先选择连续状态标签和本次单独动作标签。", "采集设置未完成", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        StartCollectionButton.IsEnabled = false;
        try
        {
            if (!await EnsureAllDevicesOnlineAsync())
            {
                UpdateCollectionStartEnabled();
                return;
            }
            UpdateCollectorBladeSettings();
            await ShowPreparationCountdownAsync();
            await Task.Run(PauseTelemetry);
            _collector.Start(port, CollectionRootText.Text);
            StopCollectionButton.IsEnabled = true;
            PortSelector.IsEnabled = false;
            StateLabelSelector.IsEnabled = false;
            EventSelector.IsEnabled = false;
            BladeModeSelector.IsEnabled = false;
            BladeCountdownText.IsEnabled = false;
            BladeTargetCountText.IsEnabled = false;
            BladeCompensationText.IsEnabled = false;
        }
        catch (Exception exception)
        {
            ResumeTelemetry();
            UpdateCollectionStartEnabled();
            MessageBox.Show(this, exception.Message, "采集启动失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void StopCollection_Click(object sender, RoutedEventArgs e) => StopCollection();

    private void StopCollection()
    {
        if (!_collector.IsRunning && _collector.SamplesPath is null)
        {
            return;
        }
        _collector.Stop();
        ResumeTelemetry();
        StartCollectionButton.IsEnabled = true;
        StopCollectionButton.IsEnabled = false;
        PortSelector.IsEnabled = true;
        StateLabelSelector.IsEnabled = true;
        EventSelector.IsEnabled = true;
        BladeModeSelector.IsEnabled = true;
        BladeCountdownText.IsEnabled = true;
        BladeTargetCountText.IsEnabled = true;
        BladeCompensationText.IsEnabled = true;
        UpdateCollectionStartEnabled();
        if (_collector.SamplesPath is not null && _collector.EventsPath is not null)
        {
            CollectionFilesText.Text =
                $"训练数据 samples: {_collector.SamplesPath}\n" +
                $"训练标签 events: {_collector.EventsPath}\n" +
                $"仅供延迟检查（不参与训练）: {_collector.TimingDiagnosticsPath}";
            RefreshCollectionLibrary();
            CollectionLibraryStatusText.Text = "新采集已加入列表，请在“我的采集数据”中勾选后进入训练";
        }
    }

    private void RefreshCollectionLibrary_Click(object sender, RoutedEventArgs e) => RefreshCollectionLibrary();

    private void RefreshCollectionLibrary()
    {
        var selectedIds = _collectionSessions.Where(item => item.IsSelected)
            .Select(item => item.SessionId)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        _collectionSessions.Clear();
        try
        {
            foreach (var session in _collectionLibrary.Load(CollectionRootText.Text))
            {
                session.IsSelected = selectedIds.Contains(session.SessionId);
                _collectionSessions.Add(session);
            }
            CollectionLibraryRootText.Text = CollectionRootText.Text;
            CollectionLibraryStatusText.Text = _collectionSessions.Count == 0
                ? "还没有完整采集会话，请先完成一次采集"
                : $"共 {_collectionSessions.Count} 次采集；勾选后可合并训练";
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            CollectionLibraryStatusText.Text = $"读取采集数据失败：{exception.Message}";
        }
    }

    private void SelectAllCollections_Click(object sender, RoutedEventArgs e)
    {
        foreach (var session in _collectionSessions)
        {
            session.IsSelected = true;
        }
        CollectionLibraryStatusText.Text = $"已选择 {_collectionSessions.Count} 次采集";
    }

    private void ClearCollectionSelection_Click(object sender, RoutedEventArgs e)
    {
        foreach (var session in _collectionSessions)
        {
            session.IsSelected = false;
        }
        CollectionLibraryStatusText.Text = "已清除选择";
    }

    private void DeleteSelectedCollections_Click(object sender, RoutedEventArgs e)
    {
        var selected = _collectionSessions.Where(item => item.IsSelected).ToArray();
        if (selected.Length == 0)
        {
            MessageBox.Show(this, "请先勾选要删除的本地采集会话。", "没有选择", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        var confirmation = MessageBox.Show(
            this,
            $"将永久删除本机选中的 {selected.Length} 次采集及其中的 samples.csv、events.csv。\n\n官方基础数据、云端已上传数据和其他未选会话不会受影响。是否继续？",
            "删除选中的本地采集数据",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);
        if (confirmation != MessageBoxResult.Yes)
        {
            return;
        }

        var errors = new List<string>();
        foreach (var session in selected)
        {
            try
            {
                _collectionLibrary.Delete(CollectionRootText.Text, session);
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidOperationException)
            {
                errors.Add($"{session.SessionId}: {exception.Message}");
            }
        }
        RefreshCollectionLibrary();
        CollectionLibraryStatusText.Text = errors.Count == 0
            ? $"已删除 {selected.Length} 次本地采集"
            : $"部分删除失败：{string.Join("；", errors)}";
    }

    private void PrepareSelectedCollections_Click(object sender, RoutedEventArgs e)
    {
        var selected = _collectionSessions.Where(item => item.IsSelected).ToArray();
        try
        {
            _preparedDataset = _collectionLibrary.Prepare(CollectionRootText.Text, selected);
            SamplesPathText.Text = _preparedDataset.SamplesPath;
            EventsPathText.Text = _preparedDataset.EventsPath;
            DatasetNameText.Text = $"玩家选定采集-{DateTime.Now:yyyy-MM-dd-HHmm}";
            _activeDataset = null;
            _activeJob = null;
            CollectionLibraryStatusText.Text =
                $"已合并 {_preparedDataset.SessionIds.Length} 次采集：样本 {_preparedDataset.SampleCount:N0}，标记 {_preparedDataset.EventCount:N0}";
            TrainingTabs.SelectedIndex = 2;
            JobStatusText.Text = "玩家数据已准备，连接服务器后即可上传训练";
            UpdateTrainEnabled();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            MessageBox.Show(this, exception.Message, "准备训练数据失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void MarkSelectedEvent_Click(object sender, RoutedEventArgs e)
    {
        if (EventSelector.SelectedItem is not TrainingEventOption marker)
        {
            MessageBox.Show(this, "请先选择动作事件。", "没有动作", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        try
        {
            _collector.MarkEvent(marker);
            CollectionStatusText.Text = $"已标记：{marker.DisplayName} ({marker.Type})";
        }
        catch (InvalidOperationException exception)
        {
            MessageBox.Show(this, exception.Message, "尚未采集", MessageBoxButton.OK, MessageBoxImage.Information);
        }
    }

    private void RestoreDefaultEvents_Click(object sender, RoutedEventArgs e)
    {
        var confirmation = MessageBox.Show(
            this,
            "将恢复所有缺失的官方默认动作；你自己新增的动作会保留。是否继续？",
            "恢复官方动作",
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);
        if (confirmation != MessageBoxResult.Yes)
        {
            return;
        }
        try
        {
            var selectedType = (EventSelector.SelectedItem as TrainingEventOption)?.Type;
            _eventOptions.Clear();
            foreach (var option in _eventCatalog.RestoreMissingDefaults())
            {
                _eventOptions.Add(option);
            }
            EventSelector.SelectedItem = _eventOptions.FirstOrDefault(item => item.Type == selectedType)
                ?? _eventOptions.FirstOrDefault();
            CollectionStatusText.Text = "缺失的官方动作已经恢复，自定义动作保持不变";
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, exception.Message, "恢复动作失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void EventSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (EventSelector.SelectedItem is TrainingEventOption marker)
        {
            _editingEvent = marker;
            _creatingEvent = false;
            EventDisplayNameText.Text = marker.DisplayName;
            EventTypeText.Text = marker.Type;
            SaveEventOptionButton.Content = "保存修改";
            DeleteEventOptionButton.IsEnabled = true;
        }
        UpdateCollectorBladeSettings();
        UpdateCollectionStartEnabled();
    }

    private void BladeSettings_Changed(object sender, RoutedEventArgs e) => UpdateCollectorBladeSettings();

    private void BladeModeSelector_SelectionChanged(object sender, SelectionChangedEventArgs e) =>
        UpdateCollectorBladeSettings();

    private void NewEventOption_Click(object sender, RoutedEventArgs e)
    {
        _editingEvent = null;
        _creatingEvent = true;
        EventSelector.SelectedIndex = -1;
        EventDisplayNameText.Text = "";
        EventTypeText.Text = NextCustomActionId();
        SaveEventOptionButton.Content = "创建动作";
        DeleteEventOptionButton.IsEnabled = false;
        EventDisplayNameText.Focus();
        CollectionStatusText.Text = "正在新建动作：填写显示名称后点击“创建动作”";
    }

    private void SaveEventOption_Click(object sender, RoutedEventArgs e)
    {
        var wasCreating = _creatingEvent;
        var group = _editingEvent?.Group ?? "custom_event";
        var option = new TrainingEventOption(
            group,
            EventTypeText.Text.Trim().ToLowerInvariant(),
            EventDisplayNameText.Text.Trim());
        if (!EventCatalogService.IsValid(option))
        {
            MessageBox.Show(
                this,
                "请填写动作的显示名称。",
                "动作信息无效",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }

        var snapshot = _eventOptions.ToArray();
        if (_creatingEvent)
        {
            _eventOptions.Add(option);
        }
        else if (_editingEvent is not null)
        {
            var existingIndex = _eventOptions.IndexOf(_editingEvent);
            if (existingIndex < 0)
            {
                MessageBox.Show(this, "当前动作已经不在列表中，请重新选择。", "无法保存", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            _eventOptions[existingIndex] = option;
        }
        else
        {
            return;
        }
        try
        {
            _eventCatalog.Save(_eventOptions);
            EventSelector.SelectedItem = option;
            CollectionStatusText.Text = wasCreating
                ? $"新动作已创建：{option.DisplayName}"
                : $"动作修改已保存：{option.DisplayName}";
            _creatingEvent = false;
            _editingEvent = option;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            RestoreEventOptions(snapshot);
            MessageBox.Show(this, exception.Message, "动作事件库保存失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void DeleteEventOption_Click(object sender, RoutedEventArgs e)
    {
        if (EventSelector.SelectedItem is not TrainingEventOption selected)
        {
            return;
        }
        if (_eventOptions.Count <= 1)
        {
            MessageBox.Show(this, "动作事件库至少要保留一个动作。", "无法删除", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        var confirmation = MessageBox.Show(
            this,
            $"只把“{selected.DisplayName}”从本机动作列表移除。\n\n不会删除已经采集的 CSV 标签、云端数据或训练模型。是否继续？",
            "从动作列表移除",
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);
        if (confirmation != MessageBoxResult.Yes)
        {
            return;
        }
        var snapshot = _eventOptions.ToArray();
        _eventOptions.Remove(selected);
        try
        {
            _eventCatalog.Save(_eventOptions);
            EventSelector.SelectedIndex = 0;
            CollectionStatusText.Text = $"已从事件库删除：{selected.DisplayName}";
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            RestoreEventOptions(snapshot);
            MessageBox.Show(this, exception.Message, "动作事件库保存失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private string NextCustomActionId()
    {
        for (var sequence = 1; sequence <= 9999; sequence++)
        {
            var candidate = $"custom_action_{sequence:D3}";
            if (_eventOptions.All(item => !item.Type.Equals(candidate, StringComparison.Ordinal)))
            {
                return candidate;
            }
        }
        throw new InvalidOperationException("自定义动作数量已达到上限。 ");
    }

    private void RestoreEventOptions(IEnumerable<TrainingEventOption> snapshot)
    {
        _eventOptions.Clear();
        foreach (var item in snapshot)
        {
            _eventOptions.Add(item);
        }
        EventSelector.SelectedItem = _editingEvent ?? _eventOptions.FirstOrDefault();
    }

    private void UpdateCollectorBladeSettings()
    {
        var selectedEvent = EventSelector?.SelectedItem as TrainingEventOption;
        var mode = (BladeModeSelector?.SelectedItem as BladeMarkingModeOption)?.Mode ?? BladeMarkingMode.Immediate;
        var countdownMs = ParseInt(BladeCountdownText?.Text, 5000);
        var targetCount = Math.Clamp(ParseInt(BladeTargetCountText?.Text, 30), 1, 500);
        var compensationMs = ParseInt(BladeCompensationText?.Text, 50);
        if (BladeCountdownPanel is not null)
        {
            BladeCountdownPanel.Visibility = mode == BladeMarkingMode.Countdown ? Visibility.Visible : Visibility.Collapsed;
        }
        if (BladeCompensationPanel is not null)
        {
            BladeCompensationPanel.Visibility = mode == BladeMarkingMode.Immediate ? Visibility.Visible : Visibility.Collapsed;
        }
        if (ManualMarkButton is not null)
        {
            ManualMarkButton.Visibility = mode == BladeMarkingMode.Immediate ? Visibility.Visible : Visibility.Collapsed;
        }
        if (CollectionModeHelpText is not null)
        {
            CollectionModeHelpText.Text = mode == BladeMarkingMode.Countdown
                ? $"自动完成 {targetCount} 次采集：每次倒计时后记录一次，并显示剩余次数。"
                : "采集期间按一次 Blade 生成一个所选动作标签；电脑按钮可作备用触发。";
        }
        _collector.ConfigureBladeMarker(selectedEvent, mode, countdownMs, compensationMs, targetCount);
    }

    private static int ParseInt(string? value, int fallback) =>
        int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed) ? parsed : fallback;

    private void Collector_StatusChanged(object? sender, ImuCollectionStatus status)
    {
        _ = Dispatcher.BeginInvoke(() =>
        {
            CollectionStatusText.Text = status.Detail;
            SampleCountText.Text =
                $"样本 {status.SampleCount:N0} · 事件 {status.EventCount:N0} · " +
                $"Tracker {status.OnlineNodes.Length}/4 · Blade {(status.BladeOnline ? "在线" : "离线")}";
            if (!status.Running && StopCollectionButton.IsEnabled)
            {
                StopCollection();
            }
        });
    }

    private void Collector_CountdownChanged(object? sender, BladeCountdownStatus status)
    {
        _ = Dispatcher.BeginInvoke(async () =>
        {
            if (status.IsCancelled)
            {
                _countdownWindow?.Close();
                _countdownWindow = null;
                return;
            }
            if (_countdownWindow is null)
            {
                _countdownWindow = new CollectionCountdownWindow { Owner = this };
                _countdownWindow.Show();
            }
            _countdownWindow.UpdateStatus(status);
            if (status.IsCompleted &&
                (status.TargetCount <= 0 || status.CompletedCount >= status.TargetCount))
            {
                var completedWindow = _countdownWindow;
                await Task.Delay(450);
                if (ReferenceEquals(_countdownWindow, completedWindow))
                {
                    completedWindow.Close();
                    _countdownWindow = null;
                }
            }
        });
    }

    private async Task<bool> EnsureAllDevicesOnlineAsync()
    {
        var snapshot = _getTelemetrySnapshot?.Invoke();
        if (snapshot is null && _refreshTelemetry is not null)
        {
            CollectionStatusText.Text = "正在刷新 Dongle、Tracker 和 Blade 在线状态……";
            await _refreshTelemetry();
            snapshot = _getTelemetrySnapshot?.Invoke();
        }
        if (_getTelemetrySnapshot is null)
        {
            return true;
        }
        if (snapshot is null)
        {
            MessageBox.Show(this, "没有收到 Dongle 的最新设备状态，请确认 Dongle 已进入橙灯采集态并点击“刷新设备状态”。",
                "无法开始采集", MessageBoxButton.OK, MessageBoxImage.Warning);
            return false;
        }
        if (snapshot.TrackerOnline < 4 || !snapshot.BladeOnline)
        {
            MessageBox.Show(this,
                $"设备未全部在线：Tracker {snapshot.TrackerOnline}/4，Blade {(snapshot.BladeOnline ? "在线" : "离线")}。\n请打开所有节点和 Blade 后刷新状态。",
                "无法开始采集", MessageBoxButton.OK, MessageBoxImage.Warning);
            return false;
        }
        return true;
    }

    private async Task ShowPreparationCountdownAsync()
    {
        _countdownWindow?.Close();
        _countdownWindow = new CollectionCountdownWindow { Owner = this };
        _countdownWindow.Show();
        for (var remaining = 2000; remaining > 0; remaining -= 1000)
        {
            _countdownWindow.UpdateStatus(new BladeCountdownStatus(
                "准备开始采集", remaining, IsGo: false, IsCompleted: false, IsPreparation: true));
            CollectionStatusText.Text = $"{remaining / 1000} 秒后开始采集，请做好准备";
            await Task.Delay(1000);
        }
        _countdownWindow.UpdateStatus(new BladeCountdownStatus(
            "准备开始采集", 0, IsGo: true, IsCompleted: false, IsPreparation: true));
        await Task.Delay(350);
        _countdownWindow.Close();
        _countdownWindow = null;
    }

    private async void RefreshCollectionStatus_Click(object sender, RoutedEventArgs e)
    {
        if (_collector.IsRunning)
        {
            _collector.RefreshStatus();
            return;
        }
        CollectionStatusText.Text = "正在刷新设备状态……";
        if (_refreshTelemetry is not null)
        {
            await _refreshTelemetry();
        }
        var snapshot = _getTelemetrySnapshot?.Invoke();
        CollectionStatusText.Text = snapshot is null
            ? "尚未收到状态，请确认 Dongle 串口和运行模式"
            : $"Tracker {snapshot.TrackerOnline}/4 · Blade {(snapshot.BladeOnline ? "在线" : "离线")}";
        if (snapshot is not null)
        {
            SampleCountText.Text = $"样本 0 · 事件 0 · Tracker {snapshot.TrackerOnline}/4 · Blade {(snapshot.BladeOnline ? "在线" : "离线")}";
        }
    }

    private void UpdatePrecollectionDeviceStatus()
    {
        if (_collector.IsRunning)
        {
            return;
        }
        var snapshot = _getTelemetrySnapshot?.Invoke();
        if (snapshot is null)
        {
            return;
        }
        SampleCountText.Text =
            $"样本 0 · 事件 0 · Tracker {snapshot.TrackerOnline}/4 · " +
            $"Blade {(snapshot.BladeOnline ? "在线" : "离线")}";
    }

    private void Collector_AutomaticSequenceCompleted(object? sender, EventArgs e)
    {
        _ = Dispatcher.BeginInvoke(() =>
        {
            CollectionStatusText.Text = "计划次数已经全部完成，正在停止并保存";
            StopCollection();
        });
    }

    private void UpdateCollectionStartEnabled()
    {
        if (StartCollectionButton is null)
        {
            return;
        }
        StartCollectionButton.IsEnabled = !_collector.IsRunning &&
            PortSelector?.SelectedItem is string &&
            StateLabelSelector?.SelectedItem is TrainingLabelOption &&
            EventSelector?.SelectedItem is TrainingEventOption;
    }

    private async void ConnectCloud_Click(object sender, RoutedEventArgs e)
    {
        await ConnectCloudAsync();
    }

    private async Task<bool> ConnectCloudAsync()
    {
        if (_api is not null && _tunnel?.IsRunning == true)
        {
            return true;
        }
        ConnectCloudButton.IsEnabled = false;
        SetCloudStatus("正在通过 SSH 连接…", "#FFF59E0B");
        try
        {
            _api?.Dispose();
            _tunnel?.Dispose();
            _tunnel = new SshTunnelService();
            var token = await _tunnel.ConnectAsync();
            _api = new CloudTrainingApiClient(_tunnel.BaseUri!, token);
            if (!await _api.CheckHealthAsync())
            {
                throw new InvalidOperationException("服务器健康检查失败。 ");
            }
            SetCloudStatus("阿里云已安全连接", "#FF2DD4BF");
            ConnectCloudButton.Content = "重新连接";
            UpdateTrainEnabled();
            await RefreshHistoryAsync(CancellationToken.None);
            return true;
        }
        catch (Exception exception)
        {
            SetCloudStatus("云端连接失败", "#FFEF4444");
            MessageBox.Show(this,
                $"{exception.Message}\n\n请先确认：网络可用、命令行可执行 ssh movetoplay-server。",
                "连接失败", MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }
        finally
        {
            ConnectCloudButton.IsEnabled = true;
        }
    }

    private void ChooseSamples_Click(object sender, RoutedEventArgs e)
    {
        var path = ChooseCsv("选择 samples.csv", SamplesPathText.Text);
        if (path is not null)
        {
            SamplesPathText.Text = path;
            UpdateTrainEnabled();
        }
    }

    private void ChooseEvents_Click(object sender, RoutedEventArgs e)
    {
        var path = ChooseCsv("选择 events.csv", EventsPathText.Text);
        if (path is not null)
        {
            EventsPathText.Text = path;
            UpdateTrainEnabled();
        }
    }

    private string? ChooseCsv(string title, string current)
    {
        var dialog = new OpenFileDialog
        {
            Title = title,
            Filter = "CSV 文件 (*.csv)|*.csv|所有文件 (*.*)|*.*",
            CheckFileExists = true,
            InitialDirectory = File.Exists(current) ? Path.GetDirectoryName(current) : null,
        };
        return dialog.ShowDialog(this) == true ? dialog.FileName : null;
    }

    private async void Train_Click(object sender, RoutedEventArgs e)
    {
        if (_preparedDataset is null || !File.Exists(SamplesPathText.Text) || !File.Exists(EventsPathText.Text))
        {
            MessageBox.Show(this, "请先到“我的采集数据”页面勾选会话并点击“使用选中数据进入训练”。", "缺少数据", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (!await ConnectCloudAsync() || _api is null)
        {
            return;
        }

        SetOperationRunning(true);
        _operationCancellation = new CancellationTokenSource();
        var cancellationToken = _operationCancellation.Token;
        try
        {
            ResetTrainingPresentation("正在准备上传数据");
            JobStatusText.Text = "正在计算文件哈希并登记数据集";
            if (_activeDataset is null || _activeDataset.Status != "uploading")
            {
                var systemConfig = await _api.GetSystemConfigAsync(cancellationToken);
                var baseDatasetId = systemConfig.OfficialDatasetId;
                if (string.IsNullOrWhiteSpace(baseDatasetId))
                {
                    throw new InvalidOperationException("服务器尚未配置只读官方训练数据集，已阻止本次训练以避免遗漏官方动作。 ");
                }
                JobStatusText.Text = $"正在把所选玩家会话叠加到官方数据集 {baseDatasetId}";
                _activeDataset = await _api.CreateDatasetAsync(
                    string.IsNullOrWhiteSpace(DatasetNameText.Text) ? $"MoveToPlay-{DateTime.Now:yyyyMMdd-HHmm}" : DatasetNameText.Text.Trim(),
                    SamplesPathText.Text, EventsPathText.Text, baseDatasetId, cancellationToken);
            }
            JobIdentityText.Text = $"DATASET {_activeDataset.Id}";
            var progress = new Progress<CloudUploadProgress>(value =>
            {
                UploadProgress.Value = value.Percent;
                UploadProgressText.Text = $"{value.Stage} · {value.CompletedBytes:N0}/{value.TotalBytes:N0} 字节 · {value.Percent:F1}%";
            });
            _activeDataset = await _api.UploadDatasetAsync(
                _activeDataset, SamplesPathText.Text, EventsPathText.Text, progress, cancellationToken);
            UploadProgress.Value = 100;
            UploadProgressText.Text = "上传完成，服务器已核对长度、SHA-256 和 CSV 表头";

            _activeJob = await _api.CreateJobAsync(_activeDataset.Id, "train", cancellationToken);
            _artifacts = [];
            ArtifactList.ItemsSource = Array.Empty<string>();
            MetricsText.Text = "模型训练中；准确率、Macro F1 和质量门禁将在训练完成后显示。";
            ExistingJobIdText.Text = _activeJob.Id;
            _history.SaveLastJob(_activeJob.Id, _activeJob.DatasetId, _activeJob.Status);
            JobIdentityText.Text = $"JOB {_activeJob.Id}\nDATASET {_activeDataset.Id}";
            _activeJob = await _api.WaitForJobAsync(_activeJob.Id, job =>
            {
                _ = Dispatcher.BeginInvoke(() => UpdateTrainingPresentation(job));
            }, cancellationToken);
            UpdateTrainingPresentation(_activeJob);
            _history.SaveLastJob(_activeJob.Id, _activeJob.DatasetId, _activeJob.Status);
            if (_activeJob.Status != "passed")
            {
                MetricsText.Text = string.IsNullOrWhiteSpace(_activeJob.Error) ? "训练未通过，请下载日志检查。" : _activeJob.Error;
            }
            await RefreshArtifactsAndMetricsAsync(cancellationToken);
            await RefreshHistoryAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            JobStatusText.Text = "已停止在本机等待；服务器任务不会被删除，可稍后按 Job ID 查询";
        }
        catch (Exception exception)
        {
            JobStatusText.Text = "操作中断，可重新点击以按服务器偏移续传";
            MetricsText.Text = exception.Message;
            SetCloudStatus("连接或任务异常", "#FFEF4444");
        }
        finally
        {
            SetOperationRunning(false);
            _operationCancellation?.Dispose();
            _operationCancellation = null;
        }
    }

    private async Task RefreshArtifactsAndMetricsAsync(CancellationToken cancellationToken)
    {
        if (_api is null || _activeJob is null)
        {
            return;
        }
        UpdateTrainingPresentation(_activeJob);
        if (_activeJob.Status is "queued" or "running")
        {
            _artifacts = [];
            ArtifactList.ItemsSource = Array.Empty<string>();
            DownloadArtifactsButton.IsEnabled = false;
            ApproveButton.IsEnabled = false;
            BuildFirmwareButton.IsEnabled = false;
            MetricsText.Text = "模型训练中；训练完成后再显示准确率、Macro F1、质量门禁和模型产物。";
            return;
        }
        _artifacts = await _api.ListArtifactsAsync(_activeJob.Id, cancellationToken);
        ArtifactList.ItemsSource = _artifacts.Select(item => $"{item.Path}  ({item.Bytes:N0} B)").ToArray();
        DownloadArtifactsButton.IsEnabled = _artifacts.Length > 0;
        ApproveButton.IsEnabled = _activeJob.Status == "passed" && string.IsNullOrWhiteSpace(_activeJob.ApprovedAt);
        BuildFirmwareButton.IsEnabled = _activeJob.Status == "passed" && !string.IsNullOrWhiteSpace(_activeJob.ApprovedAt);
        FirmwareStatusText.Text = BuildFirmwareButton.IsEnabled
            ? _activeJob.FirmwareStatus switch
            {
                "ready" => "云端固件已就绪，可以下载并校验",
                "queued" => "云端固件正在排队编译",
                "building" => $"云端固件编译中 {_activeJob.FirmwareProgressPercent:0}%",
                "failed" => $"云端固件编译失败：{_activeJob.FirmwareError}",
                _ => "模型已确认采用，可以请求云端生成固件",
            }
            : "等待训练通过并确认采用模型";

        var manifestArtifact = _artifacts.FirstOrDefault(item => item.Path == "run_manifest.json");
        if (manifestArtifact is null)
        {
            return;
        }
        var json = await _api.DownloadArtifactTextAsync(_activeJob.Id, manifestArtifact.Path, cancellationToken);
        try
        {
            var cacheDirectory = _history.CacheDirectory(_activeJob.Id);
            Directory.CreateDirectory(cacheDirectory);
            File.WriteAllText(_history.ManifestPath(_activeJob.Id), json);
            foreach (var artifact in _artifacts.Where(IsEssentialModelArtifact))
            {
                var destination = Path.GetFullPath(Path.Combine(cacheDirectory, artifact.Path.Replace('/', Path.DirectorySeparatorChar)));
                if (destination.StartsWith(cacheDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                {
                    await _api.DownloadArtifactAsync(
                        _activeJob.Id, artifact.Path, destination, artifact.Sha256, cancellationToken);
                }
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            // Cache failure must not hide an otherwise valid cloud training result.
            CollectionStatusText.Text = $"离线缓存未更新：{exception.Message}";
        }
        MetricsText.Text = MetricsFromManifest(json);
    }

    private static string MetricsFromManifest(string json)
    {
        using var document = JsonDocument.Parse(json);
        var summaries = new List<string>();
        if (document.RootElement.TryGetProperty("models", out var models))
        {
            foreach (var model in models.EnumerateArray())
            {
                var name = model.GetProperty("name").GetString() ?? "model";
                var training = model.GetProperty("training_summary");
                var accuracy = training.GetProperty("accuracy").GetDouble();
                var macroF1 = training.TryGetProperty("macro_f1", out var f1)
                    ? f1.GetDouble()
                    : training.GetProperty("report").GetProperty("macro avg").GetProperty("f1-score").GetDouble();
                var gate = model.GetProperty("quality_gate").GetProperty("ok").GetBoolean();
                var reference = model.GetProperty("reference_generated_comparison").GetProperty("ok").GetBoolean();
                summaries.Add($"{ModelChinese(name)}：准确率 {accuracy:P2} · Macro F1 {macroF1:P2} · 质量门禁 {(gate ? "通过" : "失败")} · 相对固件基线 {(reference ? "一致" : "有新模型")}");
            }
        }
        return summaries.Count == 0 ? "训练已完成，详见 run_manifest.json。" : string.Join("\n", summaries);
    }

    private async void LoadExistingJob_Click(object sender, RoutedEventArgs e)
    {
        var jobId = ExistingJobIdText.Text.Trim();
        if (jobId.Length != 32)
        {
            MessageBox.Show(this, "请输入 32 位 Job ID。", "Job ID 无效", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (!await ConnectCloudAsync() || _api is null)
        {
            return;
        }
        try
        {
            _activeJob = await _api.GetJobAsync(jobId);
            JobIdentityText.Text = $"JOB {_activeJob.Id}\nDATASET {_activeJob.DatasetId}";
            UpdateTrainingPresentation(_activeJob);
            _history.SaveLastJob(_activeJob.Id, _activeJob.DatasetId, _activeJob.Status);
            await RefreshArtifactsAndMetricsAsync(CancellationToken.None);
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "加载任务失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void LoadCachedResult()
    {
        var jobId = _history.LoadLastJobId();
        if (string.IsNullOrWhiteSpace(jobId))
        {
            return;
        }
        ExistingJobIdText.Text = jobId;
        var manifestPath = _history.ManifestPath(jobId);
        if (!File.Exists(manifestPath))
        {
            return;
        }
        try
        {
            MetricsText.Text = MetricsFromManifest(File.ReadAllText(manifestPath));
            JobStatusText.Text = "已载入本机缓存的上次训练结果（离线可查看）";
            TrainingStageText.Text = "本机缓存模型已完成";
            TrainingProgress.IsIndeterminate = false;
            TrainingProgress.Value = 100;
            TrainingProgressText.Text = "100%";
            TrainingTimeText.Text = "离线缓存 · 可连接云端刷新详情";
            JobIdentityText.Text = $"JOB {jobId} · LOCAL CACHE";
            var cacheDirectory = _history.CacheDirectory(jobId);
            ArtifactList.ItemsSource = Directory.EnumerateFiles(cacheDirectory, "*", SearchOption.AllDirectories)
                .Select(path => Path.GetRelativePath(cacheDirectory, path))
                .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
                .ToArray();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            MetricsText.Text = $"本机缓存无法读取：{exception.Message}";
        }
    }

    private void ResetTrainingPresentation(string stage)
    {
        TrainingProgress.Value = 0;
        TrainingProgress.IsIndeterminate = false;
        TrainingStageText.Text = stage;
        TrainingProgressText.Text = "0%";
        TrainingTimeText.Text = "已用 00:00 · 剩余 —";
        MetricsText.Text = "训练中；完成后显示准确率、Macro F1 与质量门禁。";
        ArtifactList.ItemsSource = Array.Empty<string>();
        DownloadArtifactsButton.IsEnabled = false;
        ApproveButton.IsEnabled = false;
    }

    private void UpdateTrainingPresentation(CloudJob job)
    {
        JobStatusText.Text = JobStatusChinese(job.Status);
        var isRunning = job.Status is "queued" or "running";
        TrainingProgress.IsIndeterminate = job.Status == "queued";
        TrainingProgress.Value = job.Status is "passed" or "validated" ? 100 : Math.Clamp(job.ProgressPercent, 0, 100);
        TrainingStageText.Text = job.Status switch
        {
            "queued" => "等待云端训练资源",
            "running" => job.ProgressDetail ?? TrainingStageChinese(job.ProgressStage),
            "passed" => "训练、评估、质量门禁和 C 数组导出全部完成",
            "validated" => "数据集校验完成",
            "failed" => "训练中断，请查看错误信息",
            _ => job.ProgressDetail ?? "等待任务状态",
        };
        TrainingProgressText.Text = job.Status is "passed" or "validated"
            ? "100%"
            : $"{Math.Clamp(job.ProgressPercent, 0, 100):0}%";
        var remaining = job.EstimatedRemainingSeconds is int seconds && isRunning
            ? FormatDuration(seconds)
            : "—";
        TrainingTimeText.Text = $"已用 {FormatDuration(job.ElapsedSeconds)} · 预计剩余 {remaining}";
        if (isRunning)
        {
            MetricsText.Text = "模型训练中；完成后再显示准确率、Macro F1 和质量门禁。";
        }
    }

    private static string FormatDuration(int seconds)
    {
        var duration = TimeSpan.FromSeconds(Math.Max(0, seconds));
        return duration.TotalHours >= 1
            ? $"{(int)duration.TotalHours:00}:{duration.Minutes:00}:{duration.Seconds:00}"
            : $"{duration.Minutes:00}:{duration.Seconds:00}";
    }

    private static string TrainingStageChinese(string? stage) => stage switch
    {
        "preparing" => "正在准备并合并训练数据",
        "validating" => "正在校验数据集",
        "validated" => "数据校验完成",
        "training_state" => "正在训练连续状态随机森林",
        "evaluating_state" => "正在评估状态模型",
        "exporting_state" => "正在导出状态模型 C 数组",
        "state_complete" => "状态模型已完成",
        "training_event" => "正在训练动作事件随机森林",
        "evaluating_event" => "正在评估动作事件模型",
        "exporting_event" => "正在导出动作模型 C 数组",
        "quality_gate" => "正在执行质量门禁",
        "finalizing" => "正在生成并校验模型产物",
        _ => "云端正在训练模型",
    };

    private async void RefreshHistory_Click(object sender, RoutedEventArgs e)
    {
        if (await ConnectCloudAsync())
        {
            await RefreshHistoryAsync(CancellationToken.None);
        }
    }

    private async Task RefreshHistoryAsync(CancellationToken cancellationToken)
    {
        if (_api is null)
        {
            return;
        }
        try
        {
            var jobsTask = _api.ListJobsAsync(cancellationToken);
            var modelsTask = _api.ListModelsAsync(cancellationToken);
            await Task.WhenAll(jobsTask, modelsTask);
            _jobHistory.Clear();
            foreach (var job in jobsTask.Result)
            {
                _jobHistory.Add(job);
            }
            _modelVersions.Clear();
            foreach (var model in modelsTask.Result)
            {
                _modelVersions.Add(model);
            }
            HistoryStatusText.Text = $"共 {_jobHistory.Count} 个任务 · {_modelVersions.Count} 个已批准模型";
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception)
        {
            HistoryStatusText.Text = $"历史列表读取失败：{exception.Message}";
        }
    }

    private void JobHistoryList_SelectionChanged(object sender, SelectionChangedEventArgs e) =>
        LoadHistoryJobButton.IsEnabled = JobHistoryList.SelectedItem is CloudJob;

    private void ModelVersionList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        var selected = ModelVersionList.SelectedItem is CloudJob;
        LoadModelVersionButton.IsEnabled = selected;
        RebuildModelFirmwareButton.IsEnabled = selected;
        RollbackModelButton.IsEnabled = selected && RollbackPortSelector.SelectedItem is string;
    }

    private async void LoadSelectedHistoryJob_Click(object sender, RoutedEventArgs e)
    {
        if (JobHistoryList.SelectedItem is CloudJob job)
        {
            await LoadCloudJobAsync(job.Id, switchToCloudTab: true);
        }
    }

    private async void LoadSelectedModel_Click(object sender, RoutedEventArgs e)
    {
        if (ModelVersionList.SelectedItem is CloudJob job)
        {
            await LoadCloudJobAsync(job.Id, switchToCloudTab: true);
        }
    }

    private async Task<bool> LoadCloudJobAsync(string jobId, bool switchToCloudTab)
    {
        if (!await ConnectCloudAsync() || _api is null)
        {
            return false;
        }
        try
        {
            _activeJob = await _api.GetJobAsync(jobId);
            ExistingJobIdText.Text = _activeJob.Id;
            JobIdentityText.Text = $"JOB {_activeJob.Id}\nDATASET {_activeJob.DatasetId}";
            UpdateTrainingPresentation(_activeJob);
            _history.SaveLastJob(_activeJob.Id, _activeJob.DatasetId, _activeJob.Status);
            await RefreshArtifactsAndMetricsAsync(CancellationToken.None);
            _firmwarePackage = null;
            FirmwareProgress.Value = 0;
            if (switchToCloudTab)
            {
                TrainingTabs.SelectedIndex = 2;
            }
            return true;
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "加载历史任务失败", MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }
    }

    private void RefreshRollbackPorts_Click(object sender, RoutedEventArgs e) => RefreshRollbackPorts();

    private void RollbackPortSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (RollbackModelButton is not null)
        {
            RollbackModelButton.IsEnabled = ModelVersionList.SelectedItem is CloudJob &&
                RollbackPortSelector.SelectedItem is string;
        }
    }

    private void RefreshRollbackPorts()
    {
        var previous = RollbackPortSelector.SelectedItem as string;
        var ports = ImuCollectionService.GetPortNames();
        RollbackPortSelector.ItemsSource = ports;
        RollbackPortSelector.SelectedItem = ports.FirstOrDefault(port =>
            port.Equals(previous, StringComparison.OrdinalIgnoreCase));
        if (RollbackPortSelector.SelectedIndex < 0 && ports.Length > 0)
        {
            RollbackPortSelector.SelectedIndex = ports.Length - 1;
        }
        RollbackModelButton.IsEnabled = ModelVersionList.SelectedItem is CloudJob &&
            RollbackPortSelector.SelectedItem is string;
    }

    private static bool IsEssentialModelArtifact(CloudArtifact artifact) =>
        artifact.Path == "run_manifest.json" ||
        ((artifact.Path.StartsWith("generated/", StringComparison.Ordinal) ||
          artifact.Path.Contains("/generated/", StringComparison.Ordinal)) &&
         (artifact.Path.EndsWith(".c", StringComparison.OrdinalIgnoreCase) ||
          artifact.Path.EndsWith(".h", StringComparison.OrdinalIgnoreCase) ||
          artifact.Path.EndsWith(".json", StringComparison.OrdinalIgnoreCase)));

    private async void DownloadArtifacts_Click(object sender, RoutedEventArgs e)
    {
        if (_api is null || _activeJob is null || _artifacts.Length == 0)
        {
            return;
        }
        using var dialog = new Forms.FolderBrowserDialog
        {
            Description = "选择模型产物保存目录",
            UseDescriptionForTitle = true,
        };
        if (dialog.ShowDialog() != Forms.DialogResult.OK)
        {
            return;
        }
        var root = Path.GetFullPath(Path.Combine(dialog.SelectedPath, $"MoveToPlay-model-{_activeJob.Id}"));
        try
        {
            DownloadArtifactsButton.IsEnabled = false;
            foreach (var artifact in _artifacts)
            {
                var destination = Path.GetFullPath(Path.Combine(root, artifact.Path.Replace('/', Path.DirectorySeparatorChar)));
                if (!destination.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException($"服务器返回了不安全的产物路径：{artifact.Path}");
                }
                JobStatusText.Text = $"正在下载 {artifact.Path}";
                await _api.DownloadArtifactAsync(_activeJob.Id, artifact.Path, destination, artifact.Sha256);
            }
            JobStatusText.Text = $"全部产物已下载：{root}";
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "下载失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            DownloadArtifactsButton.IsEnabled = true;
        }
    }

    private async void Approve_Click(object sender, RoutedEventArgs e)
    {
        if (_api is null || _activeJob is null)
        {
            return;
        }
        var confirmation = MessageBox.Show(this,
            "确认表示你已经查看准确率与质量门禁，同意把这次模型用于生成 Dongle 固件。\n\n是否采用？",
            "确认采用本次模型", MessageBoxButton.YesNo, MessageBoxImage.Question);
        if (confirmation != MessageBoxResult.Yes)
        {
            return;
        }
        try
        {
            _activeJob = await _api.ApproveAsync(_activeJob.Id, Environment.UserName);
            ApproveButton.IsEnabled = false;
            ApproveButton.Content = $"已由 {_activeJob.ApprovedBy} 确认采用";
            JobStatusText.Text = "模型已确认采用，可以生成 Dongle 固件";
            await RefreshArtifactsAndMetricsAsync(CancellationToken.None);
            await RefreshHistoryAsync(CancellationToken.None);
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "批准失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void CancelTask_Click(object sender, RoutedEventArgs e) => _operationCancellation?.Cancel();

    private void RefreshFirmwarePorts_Click(object sender, RoutedEventArgs e) => RefreshFirmwarePorts();

    private void FirmwarePortSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        FlashFirmwareButton.IsEnabled = _firmwarePackage is not null && FirmwarePortSelector.SelectedItem is string;
    }

    private void RefreshFirmwarePorts()
    {
        var previous = FirmwarePortSelector.SelectedItem as string;
        var ports = ImuCollectionService.GetPortNames();
        FirmwarePortSelector.ItemsSource = ports;
        FirmwarePortSelector.SelectedItem = ports.FirstOrDefault(port =>
            port.Equals(previous, StringComparison.OrdinalIgnoreCase));
        FlashFirmwareButton.IsEnabled = _firmwarePackage is not null && FirmwarePortSelector.SelectedItem is string;
    }

    private async void BuildFirmware_Click(object sender, RoutedEventArgs e)
    {
        if (_activeJob is null || _activeJob.Status != "passed" || string.IsNullOrWhiteSpace(_activeJob.ApprovedAt))
        {
            MessageBox.Show(this, "请先等待训练通过并点击“确认采用本次模型”。", "模型尚未确认", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        await BuildActiveFirmwareAsync();
    }

    private async Task<bool> BuildActiveFirmwareAsync(
        IProgress<FirmwareDeploymentProgress>? externalProgress = null,
        bool forceCloudRebuild = false)
    {
        if (_api is null || _activeJob is null || _activeJob.Status != "passed" || string.IsNullOrWhiteSpace(_activeJob.ApprovedAt))
        {
            return false;
        }
        _firmwareCancellation?.Cancel();
        _firmwareCancellation?.Dispose();
        _firmwareCancellation = new CancellationTokenSource();
        SetFirmwareBusy(true);
        FirmwareProgress.Value = 0;
        IProgress<FirmwareDeploymentProgress> progress = new Progress<FirmwareDeploymentProgress>(value =>
        {
            FirmwareStatusText.Text = $"{value.Stage}：{value.Detail}";
            FirmwareProgress.IsIndeterminate = value.IsIndeterminate;
            FirmwareProgress.Value = value.Percent;
            externalProgress?.Report(value);
        });
        try
        {
            if (forceCloudRebuild || _activeJob.FirmwareStatus != "ready")
            {
                FirmwareStatusText.Text = forceCloudRebuild ? "已请求云端重新编译固件" : "正在请求云端固件";
                _activeJob = await _api.RequestFirmwareAsync(
                    _activeJob.Id, forceCloudRebuild, _firmwareCancellation.Token);
            }
            if (_activeJob.FirmwareStatus is "queued" or "building")
            {
                _activeJob = await _api.WaitForFirmwareAsync(
                    _activeJob.Id,
                    job => Dispatcher.Invoke(() =>
                    {
                        var value = new FirmwareDeploymentProgress(
                            "云端编译", job.FirmwareDetail ?? "服务器正在编译 Dongle 固件",
                            Math.Clamp(job.FirmwareProgressPercent * 0.75, 0, 75),
                            job.FirmwareProgressPercent <= 2);
                        progress.Report(value);
                    }),
                    _firmwareCancellation.Token);
            }
            if (_activeJob.FirmwareStatus != "ready")
            {
                throw new InvalidOperationException(_activeJob.FirmwareError ?? "云端固件编译未成功完成。 ");
            }
            _artifacts = await _api.ListArtifactsAsync(_activeJob.Id, _firmwareCancellation.Token);
            var bundle = _artifacts.FirstOrDefault(item => item.Path == "firmware/firmware-bundle.zip")
                ?? throw new FileNotFoundException("服务器报告固件已完成，但固件包产物不存在。 ");
            var cache = Path.Combine(_history.CacheDirectory(_activeJob.Id), "firmware");
            Directory.CreateDirectory(cache);
            var bundlePath = Path.Combine(cache, "firmware-bundle.zip");
            progress.Report(new FirmwareDeploymentProgress("下载固件", "正在下载云端编译好的完整固件包", 76));
            await _api.DownloadArtifactAsync(
                _activeJob.Id, bundle.Path, bundlePath, bundle.Sha256, _firmwareCancellation.Token);
            _firmwarePackage = await _firmwareDeployment.PrepareCloudFirmwareAsync(
                _activeJob.Id,
                bundlePath,
                progress,
                _firmwareCancellation.Token);
            FirmwareStatusText.Text =
                $"云端固件已下载并校验：主程序 {_firmwarePackage.AppBytes / 1024d / 1024d:0.00} MiB；请选择蓝灯 Dongle 串口";
            RefreshFirmwarePorts();
            FlashFirmwareButton.IsEnabled = FirmwarePortSelector.SelectedItem is string;
            return true;
        }
        catch (OperationCanceledException)
        {
            FirmwareStatusText.Text = "固件下载已取消";
            return false;
        }
        catch (Exception exception)
        {
            FirmwareStatusText.Text = "云端固件准备失败";
            MessageBox.Show(this, exception.Message, "准备 Dongle 固件失败", MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }
        finally
        {
            SetFirmwareBusy(false);
        }
    }

    private async void FlashFirmware_Click(object sender, RoutedEventArgs e)
    {
        if (_firmwarePackage is null)
        {
            MessageBox.Show(this, "请先下载并校验云端生成的 Dongle 固件。", "没有固件", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        if (FirmwarePortSelector.SelectedItem is not string port)
        {
            MessageBox.Show(this, "请让 Dongle 进入蓝灯 Wi-Fi 维护模式，然后刷新并选择烧录串口。", "没有串口", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        var confirmation = MessageBox.Show(
            this,
            $"确认 Dongle 已进入蓝灯维护模式，并将通过 {port} 烧录新模型固件。\n\n烧录期间请勿拔线。是否继续？",
            "烧录 Dongle",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);
        if (confirmation != MessageBoxResult.Yes)
        {
            return;
        }
        await FlashActiveFirmwareAsync(port, showCompletionDialog: true);
    }

    private async Task<bool> FlashActiveFirmwareAsync(
        string port,
        bool showCompletionDialog,
        IProgress<FirmwareDeploymentProgress>? externalProgress = null)
    {
        if (_firmwarePackage is null)
        {
            return false;
        }
        _firmwareCancellation?.Cancel();
        _firmwareCancellation?.Dispose();
        _firmwareCancellation = new CancellationTokenSource();
        SetFirmwareBusy(true);
        FirmwareProgress.Value = 0;
        var progress = new Progress<FirmwareDeploymentProgress>(value =>
        {
            FirmwareStatusText.Text = $"{value.Stage}：{value.Detail}";
            FirmwareProgress.IsIndeterminate = value.IsIndeterminate;
            FirmwareProgress.Value = value.Percent;
            externalProgress?.Report(value);
        });
        try
        {
            await _firmwareDeployment.FlashDongleAsync(
                _firmwarePackage,
                port,
                progress,
                _firmwareCancellation.Token);
            FirmwareStatusText.Text = "烧录完成；新动作将在此固件的 Wi-Fi 映射页面中出现并默认禁用";
            if (showCompletionDialog)
            {
                MessageBox.Show(this, "新模型固件已烧录完成。请让 Dongle 重启回绿色 Play，再进入 Wi-Fi 页面配置新增动作的按键。", "烧录完成", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            return true;
        }
        catch (OperationCanceledException)
        {
            FirmwareStatusText.Text = "烧录已取消；请重新进入维护模式后重试";
            return false;
        }
        catch (Exception exception)
        {
            FirmwareStatusText.Text = "烧录失败；Dongle 保持维护模式，可重新烧录";
            MessageBox.Show(this, exception.Message, "烧录 Dongle 失败", MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }
        finally
        {
            SetFirmwareBusy(false);
        }
    }

    private async void RebuildSelectedModel_Click(object sender, RoutedEventArgs e)
    {
        if (ModelVersionList.SelectedItem is not CloudJob selected)
        {
            return;
        }
        if (!await LoadCloudJobAsync(selected.Id, switchToCloudTab: true))
        {
            return;
        }
        await BuildActiveFirmwareAsync(forceCloudRebuild: true);
    }

    private async void RollbackSelectedModel_Click(object sender, RoutedEventArgs e)
    {
        if (ModelVersionList.SelectedItem is not CloudJob selected ||
            RollbackPortSelector.SelectedItem is not string port)
        {
            MessageBox.Show(this, "请选择历史模型和蓝灯 Dongle 的烧录串口。", "回滚条件不足",
                MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        var confirmation = MessageBox.Show(
            this,
            $"将回滚到 {selected.VersionDisplay}，下载其云端固件并通过 {port} 烧录。\n\n" +
            "请确认 Dongle 已进入蓝灯维护模式，烧录期间不要拔线。是否继续？",
            "一键回滚模型",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);
        if (confirmation != MessageBoxResult.Yes)
        {
            return;
        }

        RollbackModelButton.IsEnabled = false;
        RebuildModelFirmwareButton.IsEnabled = false;
        RollbackProgress.Value = 0;
        try
        {
            RollbackStatusText.Text = "正在读取历史模型的云端固件状态";
            if (!await LoadCloudJobAsync(selected.Id, switchToCloudTab: false))
            {
                return;
            }
            var buildProgress = new Progress<FirmwareDeploymentProgress>(value =>
            {
                RollbackProgress.IsIndeterminate = value.IsIndeterminate;
                RollbackProgress.Value = value.Percent * 0.55;
                RollbackStatusText.Text = $"准备固件 · {value.Stage}：{value.Detail}";
            });
            if (!await BuildActiveFirmwareAsync(buildProgress))
            {
                RollbackStatusText.Text = "历史固件下载或校验失败，未执行烧录";
                return;
            }
            var flashProgress = new Progress<FirmwareDeploymentProgress>(value =>
            {
                RollbackProgress.IsIndeterminate = value.IsIndeterminate;
                RollbackProgress.Value = 55 + value.Percent * 0.45;
                RollbackStatusText.Text = $"烧录回滚 · {value.Stage}：{value.Detail}";
            });
            if (!await FlashActiveFirmwareAsync(port, showCompletionDialog: false, externalProgress: flashProgress))
            {
                RollbackStatusText.Text = "回滚烧录未完成，可保持蓝灯模式后重试";
                return;
            }
            if (_api is not null)
            {
                _activeJob = await _api.ActivateModelAsync(selected.Id);
            }
            RollbackProgress.IsIndeterminate = false;
            RollbackProgress.Value = 100;
            RollbackStatusText.Text = $"已回滚到 {selected.VersionDisplay}；请重启 Dongle 回绿色 Play 模式";
            await RefreshHistoryAsync(CancellationToken.None);
            MessageBox.Show(this, $"已成功回滚并烧录 {selected.VersionDisplay}。", "模型回滚完成",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception exception)
        {
            RollbackProgress.IsIndeterminate = false;
            RollbackStatusText.Text = "模型回滚失败；当前 Dongle 固件状态未被软件标记为已切换";
            MessageBox.Show(this, exception.Message, "模型回滚失败",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            RebuildModelFirmwareButton.IsEnabled = ModelVersionList.SelectedItem is CloudJob;
            RollbackModelButton.IsEnabled = ModelVersionList.SelectedItem is CloudJob &&
                RollbackPortSelector.SelectedItem is string;
        }
    }

    private void SetFirmwareBusy(bool busy)
    {
        BuildFirmwareButton.IsEnabled = !busy && _activeJob is
        { Status: "passed", ApprovedAt: not null };
        FlashFirmwareButton.IsEnabled = !busy && _firmwarePackage is not null && FirmwarePortSelector.SelectedItem is string;
        FirmwarePortSelector.IsEnabled = !busy;
    }

    private void UpdateTrainEnabled()
    {
        TrainButton.IsEnabled = _api is not null && _preparedDataset is not null &&
            File.Exists(SamplesPathText.Text) && File.Exists(EventsPathText.Text)
            && _operationCancellation is null;
    }

    private void SetOperationRunning(bool running)
    {
        TrainButton.IsEnabled = !running && _api is not null && _preparedDataset is not null &&
            File.Exists(SamplesPathText.Text) && File.Exists(EventsPathText.Text);
        CancelTaskButton.IsEnabled = running;
        ConnectCloudButton.IsEnabled = !running;
    }

    private void SetCloudStatus(string text, string color)
    {
        CloudStatusText.Text = text;
        CloudStatusDot.Fill = (Brush)new BrushConverter().ConvertFromString(color)!;
    }

    private void PauseTelemetry()
    {
        if (_telemetryPaused)
        {
            return;
        }
        _pauseTelemetry();
        _telemetryPaused = true;
    }

    private void ResumeTelemetry()
    {
        if (!_telemetryPaused)
        {
            return;
        }
        _resumeTelemetry();
        _telemetryPaused = false;
    }

    private void TrainingWindow_Closed(object? sender, EventArgs e)
    {
        _operationCancellation?.Cancel();
        _deviceStatusTimer.Stop();
        _firmwareCancellation?.Cancel();
        _firmwareCancellation?.Dispose();
        _collector.StatusChanged -= Collector_StatusChanged;
        _collector.CountdownChanged -= Collector_CountdownChanged;
        _collector.AutomaticSequenceCompleted -= Collector_AutomaticSequenceCompleted;
        _collector.Dispose();
        _countdownWindow?.Close();
        ResumeTelemetry();
        _api?.Dispose();
        _tunnel?.Dispose();
    }

    private static string JobStatusChinese(string status) => status switch
    {
        "queued" => "任务已入队，等待云端 Worker",
        "running" => "云端正在训练双随机森林并导出 C 数组",
        "passed" => "训练与质量门禁全部通过",
        "validated" => "数据集校验通过",
        "failed" => "训练失败",
        _ => status,
    };

    private static string ModelChinese(string name) => name switch
    {
        "state_rf" or "state_rf_15_full" => "状态随机森林",
        "event_rf" => "事件随机森林",
        _ => name,
    };
}
