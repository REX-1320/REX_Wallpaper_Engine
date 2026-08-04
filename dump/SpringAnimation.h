#pragma once
#include <algorithm>
#include <cmath>

class SpringAnimation {
public:
    SpringAnimation(float stiffness = 200.0f, float damping = 22.0f,
                    float mass = 1.0f, float initialValue = 0.0f)
        : m_stiffness(stiffness), m_damping(damping), m_mass(mass),
          m_target(initialValue), m_value(initialValue), m_velocity(0.0f) {}

    void SetTarget(float target) { m_target = target; }
    float GetTarget() const { return m_target; }
    float GetValue() const { return m_value; }
    bool IsResting() const { return std::abs(m_velocity) < 0.001f && std::abs(m_value - m_target) < 0.001f; }

    void Update(float deltaTime) {
        deltaTime = std::min(deltaTime, 0.05f);
        float force = -m_stiffness * (m_value - m_target) - m_damping * m_velocity;
        float acceleration = force / m_mass;
        m_velocity += acceleration * deltaTime;
        m_value += m_velocity * deltaTime;
    }

private:
    float m_stiffness, m_damping, m_mass;
    float m_target, m_value, m_velocity;
};
