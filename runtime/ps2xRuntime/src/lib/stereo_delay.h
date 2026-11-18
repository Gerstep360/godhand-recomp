#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ps2x::audio
{
    class StereoDelay
    {
    public:
        // Positive values delay the left channel to match a late right channel.
        void configure(int frames)
        {
            m_channel = frames < 0 ? 1u : 0u;
            const auto count = static_cast<size_t>(frames < 0 ? -frames : frames);
            m_history.assign(count, 0);
            m_position = 0;
        }

        void reset()
        {
            for (auto &sample : m_history)
                sample = 0;
            m_position = 0;
        }

        void apply(std::span<int16_t> interleaved)
        {
            if (m_history.empty())
                return;
            for (size_t i = 0; i + 1u < interleaved.size(); i += 2u)
            {
                const int16_t sample = interleaved[i + m_channel];
                interleaved[i + m_channel] = m_history[m_position];
                m_history[m_position] = sample;
                if (++m_position == m_history.size())
                    m_position = 0;
            }
        }

    private:
        std::vector<int16_t> m_history;
        size_t m_channel = 0;
        size_t m_position = 0;
    };
}
