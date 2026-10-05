/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <wtf/MonotonicTime.h>
#include <wtf/Seconds.h>

namespace WebCore {

// What one frame asked this port to draw, and how long the port spent on it.
// DrawingAreaHaiku turns this on when SUMMIT_FRAME_STATS is set and reports it
// with the frame line, then clears it (docs/performance.md). Painting happens
// on the web process's main thread, so a plain global is enough; while it is
// off every drawing call pays one predictable branch and nothing else.
struct PaintStatisticsHaiku {
    bool enabled { false };
    unsigned glyphRuns { 0 };
    unsigned glyphs { 0 };
    unsigned images { 0 };
    unsigned fills { 0 };
    unsigned strokes { 0 };
    unsigned clips { 0 };
    unsigned stateChanges { 0 };
    // Times the port asked app_server for something and waited for the answer.
    unsigned roundTrips { 0 };
    unsigned clipQueries { 0 };
    Seconds roundTripTime;
    Seconds glyphTime;
    Seconds imageTime;
    Seconds stateTime;
    Seconds fillTime;
    Seconds clipTime;
    Seconds saveTime;
    // All time spent inside the port's drawing calls, counted once even when
    // one of them calls another. What painting costs beyond this is WebCore
    // deciding what to draw.
    Seconds primitiveTime;
    unsigned depth { 0 };

    void clear()
    {
        bool wasEnabled = enabled;
        *this = PaintStatisticsHaiku { };
        enabled = wasEnabled;
    }
};

inline PaintStatisticsHaiku& paintStatisticsHaiku()
{
    static PaintStatisticsHaiku statistics;
    return statistics;
}

// Adds the time it covers to one of the tallies, and costs nothing while the
// statistics are off.
class PaintTimerHaiku {
public:
    explicit PaintTimerHaiku(Seconds& total)
        : m_total(paintStatisticsHaiku().enabled ? &total : nullptr)
    {
        if (m_total)
            m_started = MonotonicTime::now();
    }

    ~PaintTimerHaiku()
    {
        if (m_total)
            *m_total += MonotonicTime::now() - m_started;
    }

private:
    Seconds* m_total;
    MonotonicTime m_started;
};

// Measures a drawing call, and only the outermost one when they nest.
class PrimitiveTimerHaiku {
public:
    PrimitiveTimerHaiku()
    {
        auto& statistics = paintStatisticsHaiku();
        if (!statistics.enabled || statistics.depth++)
            return;
        m_outermost = true;
        m_started = MonotonicTime::now();
    }

    ~PrimitiveTimerHaiku()
    {
        auto& statistics = paintStatisticsHaiku();
        if (!statistics.enabled)
            return;
        --statistics.depth;
        if (m_outermost)
            statistics.primitiveTime += MonotonicTime::now() - m_started;
    }

private:
    MonotonicTime m_started;
    bool m_outermost { false };
};

} // namespace WebCore
