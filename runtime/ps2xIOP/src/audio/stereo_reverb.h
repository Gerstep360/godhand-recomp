#pragma once

#include "stereo_mixer.h"

#include <array>
#include <cstdint>
#include <span>

namespace ps2x::audio
{
    struct ReverbChannel
    {
        int32_t inputGain;
        int32_t sameDestination, sameSource;
        int32_t crossDestination, crossSource;
        std::array<int32_t, 4> combOffsets;
        std::array<int32_t, 2> allPassOffsets;
    };

    struct ReverbWork
    {
        std::span<uint16_t> delayLine;
        uint32_t start;
        bool write;
        int32_t iirGain, wallGain;
        std::array<int32_t, 4> combGains;
        std::array<int32_t, 2> allPassGains, allPassDelays;
        std::array<ReverbChannel, 2> channels;
    };

    class StereoReverb
    {
    public:
        using Frame = std::array<int32_t, 2>;

        template <class GetWork>
        Frame process(const Frame &wet, GetWork readWork)
        {
            const uint32_t pos = static_cast<uint32_t>(m_frame++ & kHistoryMask);
            const uint32_t center = (pos - 19u) & kHistoryMask;
            for (size_t channel = 0; channel < 2u; ++channel)
                m_input[channel][pos] = static_cast<int16_t>(wet[channel]);

            // The interpolated 24 kHz output is zero-stuffed. On this phase,
            // only the center tap meets a nonzero history sample.
            if ((pos & 1u) == 0u)
            {
                m_output[0][pos] = m_output[1][pos] = 0;
                return {m_output[0][center], m_output[1][center]};
            }

            Frame input{};
            for (size_t channel = 0; channel < 2u; ++channel)
                input[channel] = saturate((filter(m_input[channel], pos) + 0x4000 * m_input[channel][center]) >> 15);
            const Frame lowRate = runDelayLine(input, readWork());
            Frame out{};
            for (size_t channel = 0; channel < 2u; ++channel)
            {
                m_output[channel][pos] = static_cast<int16_t>(lowRate[channel]);
                out[channel] = saturate(filter(m_output[channel], pos) >> 14);
            }
            return out;
        }

    private:
        using History = std::array<int16_t, 64>;
        static constexpr uint32_t kHistoryMask = 63u;

        static int32_t filter(const History &history, uint32_t pos)
        {
            constexpr std::array<int32_t, 10> weights{-1, 2, -10, 35, -103, 266, -616, 1332, -2960, 10246};
            int32_t sum = 0;
            for (uint32_t tap = 0; tap < weights.size(); ++tap)
                sum += weights[tap] * (history[(pos - 2u * tap) & kHistoryMask] + history[(pos - 38u + 2u * tap) & kHistoryMask]);
            return sum;
        }

        Frame runDelayLine(const Frame &input, const ReverbWork &work)
        {
            if (work.delayLine.empty())
                return {};
            const uint32_t size = static_cast<uint32_t>(work.delayLine.size());
            if (m_position < work.start || m_position - work.start >= size)
                m_position = work.start;
            const int32_t position = static_cast<int32_t>(m_position - work.start);
            auto cell = [&](int32_t offset) -> uint16_t & {
                const int32_t index = position + offset;
                if (static_cast<uint32_t>(index) < size)
                    return work.delayLine[index];
                const int32_t wrapped = index % static_cast<int32_t>(size);
                return work.delayLine[wrapped < 0 ? wrapped + size : wrapped];
            };
            auto read = [&](int32_t offset) { return static_cast<int16_t>(cell(offset)); };
            auto write = [&](int32_t offset, int32_t value) { cell(offset) = static_cast<uint16_t>(saturate(value)); };

            if (work.write)
            {
                for (size_t channel = 0; channel < 2u; ++channel)
                {
                    const auto &tap = work.channels[channel];
                    const int32_t sample = scale(tap.inputGain, input[channel]);
                    auto reflect = [&](int32_t destination, int32_t source) {
                        const int32_t previous = read(destination - 1);
                        const int32_t next = saturate(sample + scale(read(source), work.wallGain) - previous);
                        write(destination, scale(next, work.iirGain) + previous);
                    };
                    reflect(tap.sameDestination, tap.sameSource);
                    reflect(tap.crossDestination, tap.crossSource);
                }
            }
            Frame result{};
            for (size_t channel = 0; channel < 2u; ++channel)
            {
                const auto &tap = work.channels[channel];
                int32_t sample = 0;
                for (size_t comb = 0; comb < work.combGains.size(); ++comb)
                    sample += scale(work.combGains[comb], read(tap.combOffsets[comb]));
                sample = saturate(sample);
                for (size_t filter = 0; filter < work.allPassGains.size(); ++filter)
                {
                    const int32_t destination = tap.allPassOffsets[filter];
                    const int32_t delay = read(destination - work.allPassDelays[filter]);
                    const int32_t gain = work.allPassGains[filter];
                    sample = saturate(sample - scale(gain, delay));
                    if (work.write)
                        write(destination, sample);
                    sample = saturate(scale(sample, gain) + delay);
                }
                result[channel] = sample;
            }
            if (++m_position == work.start + size)
                m_position = work.start;
            return result;
        }

        std::array<History, 2> m_input{};
        std::array<History, 2> m_output{};
        uint32_t m_position = 0u;
        uint64_t m_frame = 0u;
    };
}
