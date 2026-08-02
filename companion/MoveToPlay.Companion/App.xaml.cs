using System.IO;
using System.Windows;
using System.Windows.Threading;

namespace MoveToPlay.Companion;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += OnDispatcherUnhandledException;
    }

    private static void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        var errorText = e.Exception.ToString();
        try
        {
            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "companion-error.log"), errorText);
        }
        catch
        {
            // Logging must never hide the original UI error.
        }

        MessageBox.Show(
            $"MoveToPlay Companion 遇到错误：\n\n{e.Exception.GetBaseException().Message}",
            "MoveToPlay Companion",
            MessageBoxButton.OK,
            MessageBoxImage.Error);
        e.Handled = true;
    }
}
