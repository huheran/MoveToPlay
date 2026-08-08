using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace MoveToPlay.Companion.Models;

public sealed class LocalCollectionSession : INotifyPropertyChanged
{
    private bool _isSelected;

    public required string SessionId { get; init; }
    public required string DirectoryPath { get; init; }
    public required string SamplesPath { get; init; }
    public required string EventsPath { get; init; }
    public required DateTime CreatedAt { get; init; }
    public required long SampleCount { get; init; }
    public required int EventCount { get; init; }
    public required string ActionsDisplay { get; init; }
    public required string SizeDisplay { get; init; }

    public bool IsSelected
    {
        get => _isSelected;
        set
        {
            if (_isSelected == value)
            {
                return;
            }
            _isSelected = value;
            OnPropertyChanged();
        }
    }

    public string CreatedDisplay => CreatedAt.ToString("yyyy-MM-dd HH:mm:ss");

    public string CountsDisplay => $"样本 {SampleCount:N0} · 标记 {EventCount:N0}";

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

public sealed record PreparedCollectionDataset(
    string DirectoryPath,
    string SamplesPath,
    string EventsPath,
    string[] SessionIds,
    long SampleCount,
    int EventCount);
