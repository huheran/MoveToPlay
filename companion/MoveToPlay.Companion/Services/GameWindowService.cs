using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class GameWindowService
{
    private delegate bool EnumWindowsProc(nint hwnd, nint lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    public readonly record struct WindowBounds(int Left, int Top, int Width, int Height);

    public IReadOnlyList<GameWindowTarget> GetCandidateWindows()
    {
        var result = new List<GameWindowTarget>
        {
            new(nint.Zero, "桌面预览（未绑定游戏）", "Preview"),
        };

        EnumWindows((hwnd, unused) =>
        {
            if (!IsWindowVisible(hwnd) || GetWindowTextLength(hwnd) == 0)
            {
                return true;
            }

            if (!TryGetClientBounds(hwnd, out var bounds) || bounds.Width < 800 || bounds.Height < 500)
            {
                return true;
            }

            var titleBuilder = new StringBuilder(512);
            _ = GetWindowText(hwnd, titleBuilder, titleBuilder.Capacity);
            var title = titleBuilder.ToString().Trim();
            if (string.IsNullOrWhiteSpace(title))
            {
                return true;
            }

            _ = GetWindowThreadProcessId(hwnd, out var processId);
            try
            {
                var process = Process.GetProcessById((int)processId);
                if (process.ProcessName.Equals("MoveToPlay.Companion", StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }

                result.Add(new GameWindowTarget(hwnd, title, process.ProcessName));
            }
            catch
            {
                // The process may exit while windows are enumerated.
            }

            return true;
        }, nint.Zero);

        return result
            .Skip(1)
            .OrderBy(target => target.ProcessName)
            .ThenBy(target => target.Title)
            .Prepend(result[0])
            .ToList();
    }

    public GameWindowTarget? FindProfileWindow(IEnumerable<GameWindowTarget> candidates, GameProfile profile)
    {
        var names = profile.ProcessNames.ToHashSet(StringComparer.OrdinalIgnoreCase);
        return candidates.FirstOrDefault(candidate =>
            candidate.Handle != nint.Zero
            && (names.Contains(candidate.ProcessName)
                || profile.WindowMatchRules.Any(rule => MatchesRule(candidate, rule))));
    }

    private static bool MatchesRule(GameWindowTarget candidate, WindowMatchRule rule)
    {
        if (rule.ProcessNames.Length == 0 && rule.TitleKeywords.Length == 0)
        {
            return false;
        }

        var processMatches = rule.ProcessNames.Length == 0
                             || rule.ProcessNames.Contains(candidate.ProcessName, StringComparer.OrdinalIgnoreCase);
        var titleMatches = rule.TitleKeywords.Length == 0
                           || rule.TitleKeywords.Any(keyword =>
                               !string.IsNullOrWhiteSpace(keyword)
                               && candidate.Title.Contains(keyword, StringComparison.OrdinalIgnoreCase));
        return processMatches && titleMatches;
    }

    public bool TryGetClientBounds(nint hwnd, out WindowBounds bounds)
    {
        bounds = default;
        if (hwnd == nint.Zero || !IsWindow(hwnd) || !GetClientRect(hwnd, out var clientRect))
        {
            return false;
        }

        var topLeft = new Point { X = clientRect.Left, Y = clientRect.Top };
        if (!ClientToScreen(hwnd, ref topLeft))
        {
            return false;
        }

        var width = clientRect.Right - clientRect.Left;
        var height = clientRect.Bottom - clientRect.Top;
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        bounds = new WindowBounds(topLeft.X, topLeft.Y, width, height);
        return true;
    }

    public bool IsTargetForeground(nint hwnd)
    {
        if (hwnd == nint.Zero)
        {
            return true;
        }

        var foreground = GetForegroundWindow();
        if (foreground == hwnd)
        {
            return true;
        }

        _ = GetWindowThreadProcessId(hwnd, out var targetProcess);
        _ = GetWindowThreadProcessId(foreground, out var foregroundProcess);
        return targetProcess != 0 && targetProcess == foregroundProcess;
    }

    public void PlaceTopmost(nint overlayHandle, WindowBounds bounds)
    {
        _ = SetWindowPos(
            overlayHandle,
            new nint(-1),
            bounds.Left,
            bounds.Top,
            bounds.Width,
            bounds.Height,
            0x0010 | 0x0040);
    }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, nint lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(nint hwnd);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(nint hwnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(nint hwnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    private static extern int GetWindowTextLength(nint hwnd);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool GetClientRect(nint hwnd, out Rect rect);

    [DllImport("user32.dll")]
    private static extern bool ClientToScreen(nint hwnd, ref Point point);

    [DllImport("user32.dll")]
    private static extern nint GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(nint hwnd, nint insertAfter, int x, int y, int width, int height, uint flags);
}
