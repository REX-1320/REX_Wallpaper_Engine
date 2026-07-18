#include "shared/Stopwatch.hpp"

namespace rex::shared {

Stopwatch::Stopwatch() {
    Reset();
}

void Stopwatch::Start() {
    if (m_running) return;
    m_startTime = Clock::now();
    m_running = true;
}

void Stopwatch::Stop() {
    if (!m_running) return;
    m_stopTime = Clock::now();
    m_running = false;
}

void Stopwatch::Reset() {
    m_startTime = Clock::now();
    m_stopTime = m_startTime;
    m_running = false;
}

double Stopwatch::ElapsedMilliseconds() const {
    auto end = m_running ? Clock::now() : m_stopTime;
    return std::chrono::duration<double, std::milli>(end - m_startTime).count();
}

double Stopwatch::ElapsedSeconds() const {
    return ElapsedMilliseconds() / 1000.0;
}

} // namespace rex::shared
