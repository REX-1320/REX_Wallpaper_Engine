#pragma once

#include <chrono>

namespace rex::shared {

class Stopwatch {
public:
    Stopwatch();

    void Start();
    void Stop();
    void Reset();

    double ElapsedMilliseconds() const;
    double ElapsedSeconds() const;
    bool IsRunning() const { return m_running; }

private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    TimePoint m_startTime;
    TimePoint m_stopTime;
    bool m_running = false;
};

} // namespace rex::shared
