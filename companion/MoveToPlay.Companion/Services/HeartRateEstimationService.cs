namespace MoveToPlay.Companion.Services;

public static class HeartRateEstimationService
{
    public static int EstimatePostWorkoutBpm(double calories,
                                             double activeMinutes,
                                             double averageIntensityPercent)
    {
        var calorieRate = activeMinutes >= 0.25 ? Math.Max(0.0, calories) / activeMinutes : 0.0;
        var estimate = 78.0 +
                       Math.Clamp(averageIntensityPercent, 0.0, 100.0) * 0.68 +
                       Math.Clamp(calorieRate, 0.0, 15.0) * 1.35 +
                       Math.Clamp(activeMinutes, 0.0, 30.0) * 0.12;
        return Math.Clamp((int)Math.Round(estimate), 80, 180);
    }
}
