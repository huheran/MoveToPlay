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
    private readonly ImuCollectionService _collector = new();
    private readonly EventCatalogService _eventCatalog = new();
    private readonly ObservableCollection<TrainingEventOption> _eventOptions = [];
    private readonly TrainingHistoryService _history = new();
    private SshTunnelService? _tunnel;
    private CloudTrainingApiClient? _api;
    private CancellationTokenSource? _operationCancellation;
    private CloudDataset? _activeDataset;
    private CloudJob? _activeJob;
    private CloudArtifact[] _artifacts = [];
    private TrainingEventOption? _editingEvent;
    private bool _creatingEvent;
    private bool _telemetryPaused;

    public TrainingWindow(Action pauseTelemetry, Action resumeTelemetry)
    {
        _pauseTelemetry = pauseTelemetry;
        _resumeTelemetry = resumeTelemetry;
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
                argument.Equals("--training-tab=cloud", StringComparison.OrdinalIgnoreCase)))
        {
            TrainingTabs.SelectedIndex = 1;
        }
        CollectionRootText.Text = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "MoveToPlay", "collections");
        DatasetNameText.Text = $"决赛演示采集-{DateTime.Now:yyyy-MM-dd-HHmm}";
        UpdateCollectorBladeSettings();
        _collector.StatusChanged += Collector_StatusChanged;
        Loaded += (_, _) =>
        {
            RefreshPorts();
            LoadCachedResult();
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
    }

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
        }
    }

    private void StateLabelSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (StateLabelSelector.SelectedItem is TrainingLabelOption label)
        {
            _collector.SetStateLabel(label.Id);
        }
    }

    private void StartCollection_Click(object sender, RoutedEventArgs e)
    {
        if (PortSelector.SelectedItem is not string port)
        {
            MessageBox.Show(this, "请先选择 Dongle 串口。", "无法开始采集", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        try
        {
            UpdateCollectorBladeSettings();
            PauseTelemetry();
            _collector.Start(port, CollectionRootText.Text);
            StartCollectionButton.IsEnabled = false;
            StopCollectionButton.IsEnabled = true;
            PortSelector.IsEnabled = false;
        }
        catch (Exception exception)
        {
            ResumeTelemetry();
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
        if (_collector.SamplesPath is not null && _collector.EventsPath is not null)
        {
            SamplesPathText.Text = _collector.SamplesPath;
            EventsPathText.Text = _collector.EventsPath;
            CollectionFilesText.Text = $"samples: {_collector.SamplesPath}\nevents: {_collector.EventsPath}";
            UpdateTrainEnabled();
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
        var countdownMs = ParseInt(BladeCountdownText?.Text, 1000);
        var compensationMs = ParseInt(BladeCompensationText?.Text, 50);
        if (BladeCountdownPanel is not null)
        {
            BladeCountdownPanel.Visibility = mode == BladeMarkingMode.Countdown ? Visibility.Visible : Visibility.Collapsed;
        }
        if (BladeCompensationPanel is not null)
        {
            BladeCompensationPanel.Visibility = mode == BladeMarkingMode.Immediate ? Visibility.Visible : Visibility.Collapsed;
        }
        _collector.ConfigureBladeMarker(selectedEvent, mode, countdownMs, compensationMs);
    }

    private static int ParseInt(string? value, int fallback) =>
        int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed) ? parsed : fallback;

    private void Collector_StatusChanged(object? sender, ImuCollectionStatus status)
    {
        _ = Dispatcher.BeginInvoke(() =>
        {
            CollectionStatusText.Text = status.Detail;
            var nodes = status.OnlineNodes.Length == 0 ? "—" : string.Join(", ", status.OnlineNodes);
            SampleCountText.Text = $"样本 {status.SampleCount:N0} · 事件 {status.EventCount:N0} · 在线节点 {nodes}";
            if (!status.Running && StopCollectionButton.IsEnabled)
            {
                StopCollection();
            }
        });
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
        if (!File.Exists(SamplesPathText.Text) || !File.Exists(EventsPathText.Text))
        {
            MessageBox.Show(this, "请选择有效的 samples.csv 和 events.csv。", "缺少数据", MessageBoxButton.OK, MessageBoxImage.Warning);
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
            JobStatusText.Text = "正在计算文件哈希并登记数据集";
            if (_activeDataset is null || _activeDataset.Status != "uploading")
            {
                var baseDatasetId = _history.LoadLastPassedDatasetId();
                if (!string.IsNullOrWhiteSpace(baseDatasetId))
                {
                    try
                    {
                        var baseDataset = await _api.GetDatasetAsync(baseDatasetId, cancellationToken);
                        if (!string.Equals(baseDataset.Status, "ready", StringComparison.Ordinal))
                        {
                            baseDatasetId = null;
                        }
                    }
                    catch (Exception exception) when (exception is InvalidOperationException or HttpRequestException)
                    {
                        baseDatasetId = null;
                    }
                }
                if (!string.IsNullOrWhiteSpace(baseDatasetId))
                {
                    JobStatusText.Text = $"正在登记增量数据集，基础数据集 {baseDatasetId}";
                }
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
            ExistingJobIdText.Text = _activeJob.Id;
            _history.SaveLastJob(_activeJob.Id, _activeJob.DatasetId, _activeJob.Status);
            JobIdentityText.Text = $"JOB {_activeJob.Id}\nDATASET {_activeDataset.Id}";
            _activeJob = await _api.WaitForJobAsync(_activeJob.Id, job =>
            {
                _ = Dispatcher.BeginInvoke(() => JobStatusText.Text = JobStatusChinese(job.Status));
            }, cancellationToken);
            JobStatusText.Text = JobStatusChinese(_activeJob.Status);
            _history.SaveLastJob(_activeJob.Id, _activeJob.DatasetId, _activeJob.Status);
            if (_activeJob.Status != "passed")
            {
                MetricsText.Text = string.IsNullOrWhiteSpace(_activeJob.Error) ? "训练未通过，请下载日志检查。" : _activeJob.Error;
            }
            await RefreshArtifactsAndMetricsAsync(cancellationToken);
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
        _artifacts = await _api.ListArtifactsAsync(_activeJob.Id, cancellationToken);
        ArtifactList.ItemsSource = _artifacts.Select(item => $"{item.Path}  ({item.Bytes:N0} B)").ToArray();
        DownloadArtifactsButton.IsEnabled = _artifacts.Length > 0;
        ApproveButton.IsEnabled = _activeJob.Status == "passed" && string.IsNullOrWhiteSpace(_activeJob.ApprovedAt);

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
            JobStatusText.Text = JobStatusChinese(_activeJob.Status);
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
            "批准表示你已经查看准确率与质量门禁，同意将这次训练结果作为可下发模型。\n\n是否批准？",
            "人工批准模型", MessageBoxButton.YesNo, MessageBoxImage.Question);
        if (confirmation != MessageBoxResult.Yes)
        {
            return;
        }
        try
        {
            _activeJob = await _api.ApproveAsync(_activeJob.Id, Environment.UserName);
            ApproveButton.IsEnabled = false;
            ApproveButton.Content = $"已由 {_activeJob.ApprovedBy} 批准";
            JobStatusText.Text = "模型已人工批准，可进入固件集成阶段";
            await RefreshArtifactsAndMetricsAsync(CancellationToken.None);
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "批准失败", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void CancelTask_Click(object sender, RoutedEventArgs e) => _operationCancellation?.Cancel();

    private void UpdateTrainEnabled()
    {
        TrainButton.IsEnabled = _api is not null && File.Exists(SamplesPathText.Text) && File.Exists(EventsPathText.Text)
            && _operationCancellation is null;
    }

    private void SetOperationRunning(bool running)
    {
        TrainButton.IsEnabled = !running && _api is not null && File.Exists(SamplesPathText.Text) && File.Exists(EventsPathText.Text);
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
        _collector.StatusChanged -= Collector_StatusChanged;
        _collector.Dispose();
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
