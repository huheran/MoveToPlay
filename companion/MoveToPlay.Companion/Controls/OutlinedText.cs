using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace MoveToPlay.Companion.Controls;

/// <summary>
/// Draws the outline around glyph geometry itself, so HUD text stays readable
/// without introducing a rectangular background panel.
/// </summary>
public sealed class OutlinedText : FrameworkElement
{
    public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
        nameof(Text),
        typeof(string),
        typeof(OutlinedText),
        new FrameworkPropertyMetadata(string.Empty, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty FontFamilyProperty = DependencyProperty.Register(
        nameof(FontFamily),
        typeof(FontFamily),
        typeof(OutlinedText),
        new FrameworkPropertyMetadata(SystemFonts.MessageFontFamily, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty FontSizeProperty = DependencyProperty.Register(
        nameof(FontSize),
        typeof(double),
        typeof(OutlinedText),
        new FrameworkPropertyMetadata(12.0, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty FontWeightProperty = DependencyProperty.Register(
        nameof(FontWeight),
        typeof(FontWeight),
        typeof(OutlinedText),
        new FrameworkPropertyMetadata(FontWeights.Normal, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty ForegroundProperty = DependencyProperty.Register(
        nameof(Foreground),
        typeof(Brush),
        typeof(OutlinedText),
        new FrameworkPropertyMetadata(Brushes.White, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(
        nameof(Stroke),
        typeof(Brush),
        typeof(OutlinedText),
        new FrameworkPropertyMetadata(Brushes.Black, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty StrokeThicknessProperty = DependencyProperty.Register(
        nameof(StrokeThickness),
        typeof(double),
        typeof(OutlinedText),
        new FrameworkPropertyMetadata(1.4, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

    public string Text
    {
        get => (string)GetValue(TextProperty);
        set => SetValue(TextProperty, value);
    }

    public FontFamily FontFamily
    {
        get => (FontFamily)GetValue(FontFamilyProperty);
        set => SetValue(FontFamilyProperty, value);
    }

    public double FontSize
    {
        get => (double)GetValue(FontSizeProperty);
        set => SetValue(FontSizeProperty, value);
    }

    public FontWeight FontWeight
    {
        get => (FontWeight)GetValue(FontWeightProperty);
        set => SetValue(FontWeightProperty, value);
    }

    public Brush Foreground
    {
        get => (Brush)GetValue(ForegroundProperty);
        set => SetValue(ForegroundProperty, value);
    }

    public Brush Stroke
    {
        get => (Brush)GetValue(StrokeProperty);
        set => SetValue(StrokeProperty, value);
    }

    public double StrokeThickness
    {
        get => (double)GetValue(StrokeThicknessProperty);
        set => SetValue(StrokeThicknessProperty, value);
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        var (geometry, outlinePen) = CreateGeometryAndPen();
        var bounds = geometry.GetRenderBounds(outlinePen);
        if (bounds.IsEmpty)
        {
            return new Size(0, 0);
        }

        var padding = GetRenderPadding();
        return new Size(
            Math.Ceiling(bounds.Width + padding * 2.0),
            Math.Ceiling(bounds.Height + padding * 2.0));
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);

        var (geometry, outlinePen) = CreateGeometryAndPen();
        var bounds = geometry.GetRenderBounds(outlinePen);
        if (bounds.IsEmpty)
        {
            return;
        }

        var padding = GetRenderPadding();
        drawingContext.PushTransform(new TranslateTransform(padding - bounds.Left, padding - bounds.Top));

        // Paint the outline first and restore the fill afterwards. A single
        // fill-and-stroke draw lets the inner half of the pen cover thin
        // glyphs, which is especially noticeable on small Chinese text.
        drawingContext.DrawGeometry(null, outlinePen, geometry);
        drawingContext.DrawGeometry(Foreground ?? Brushes.White, null, geometry);
        drawingContext.Pop();
    }

    private (Geometry Geometry, Pen OutlinePen) CreateGeometryAndPen()
    {
        var formatted = CreateFormattedText();
        var geometry = formatted.BuildGeometry(new Point(0, 0));
        var outlinePen = new Pen(Stroke ?? Brushes.Black, Math.Max(0, StrokeThickness))
        {
            LineJoin = PenLineJoin.Round,
        };

        return (geometry, outlinePen);
    }

    private double GetRenderPadding() => Math.Max(1.0, Math.Ceiling(Math.Max(0, StrokeThickness) / 2.0 + 1.0));

    private FormattedText CreateFormattedText()
    {
        var pixelsPerDip = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        return new FormattedText(
            Text ?? string.Empty,
            CultureInfo.CurrentUICulture,
            FlowDirection.LeftToRight,
            new Typeface(FontFamily, FontStyles.Normal, FontWeight, FontStretches.Normal),
            Math.Max(1.0, FontSize),
            Foreground ?? Brushes.White,
            pixelsPerDip);
    }
}
