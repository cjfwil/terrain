#pragma once

static LARGE_INTEGER qpc_freq;

float qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    return (float)(((double)(b.QuadPart - a.QuadPart) * 1000.0) / (double)qpc_freq.QuadPart);
}

struct
{
    LARGE_INTEGER t0, t1, t2, t3;

    float fps_1pct_low;
    float fps_01pct_low;
    float fps_peak;
    float fps_min;
    float fps_max;
    
    float update_ms;
    float render_ms;
    float present_ms;
    float frame_ms;
    
    static const int count = 256;
    float frameRateHistory[count] = {};

    
    

    void update_fps()
    {
        fps_1pct_low = findPercentile(frameRateHistory, 0.01f);
        fps_01pct_low = findPercentile(frameRateHistory, 0.001f);
        fps_peak = findPercentile(frameRateHistory, 0.999f);
        fps_min = findPercentile(frameRateHistory, 0.0f);
        fps_max = findPercentile(frameRateHistory, 1.0f);
    }

    float findPercentile(float *data, float percentile)
    {
        float temp[count];
        for (int i = 0; i < count; i++)
            temp[i] = data[i];

        // Simple bubble sort (fast enough for 256 elements)
        for (int i = 0; i < count - 1; i++)
            for (int j = 0; j < count - i - 1; j++)
                if (temp[j] > temp[j + 1])
                {
                    float t = temp[j];
                    temp[j] = temp[j + 1];
                    temp[j + 1] = t;
                }

        int index = (int)(percentile * count);
        if (index < 0)
            index = 0;
        if (index >= count)
            index = count - 1;

        return temp[index];
    }
} profiling;
