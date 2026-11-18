#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#include <cstring>
#include <emmintrin.h>
#endif

namespace ps2x::audio
{
    inline int32_t scale(int32_t sample, int32_t gain)
    {
        return static_cast<int32_t>((static_cast<int64_t>(sample) * gain) >> 15);
    }

    inline int32_t saturate(int32_t sample)
    {
        return std::clamp(sample, -32768, 32767);
    }

    inline int32_t interpolate(const std::array<int16_t, 4> &samples, const std::array<int16_t, 4> &weights)
    {
#if defined(_M_X64) || defined(__x86_64__)
        int64_t packedSamples;
        int64_t packedWeights;
        std::memcpy(&packedSamples, samples.data(), sizeof(packedSamples));
        std::memcpy(&packedWeights, weights.data(), sizeof(packedWeights));
        const __m128i s = _mm_cvtsi64_si128(packedSamples);
        const __m128i w = _mm_cvtsi64_si128(packedWeights);
        const __m128i products = _mm_unpacklo_epi16(_mm_mullo_epi16(s, w), _mm_mulhi_epi16(s, w));
        // Truncate each product at Q15 before summing, including negative taps.
        const __m128i taps = _mm_srai_epi32(products, 15);
        const __m128i pairs = _mm_add_epi32(taps, _mm_srli_si128(taps, 8));
        return _mm_cvtsi128_si32(_mm_add_epi32(pairs, _mm_srli_si128(pairs, 4)));
#else
        int32_t sum = 0;
        for (size_t tap = 0; tap < samples.size(); ++tap)
            sum += scale(samples[tap], weights[tap]);
        return sum;
#endif
    }

    struct Sends
    {
        bool dry;
        bool wet;
    };

    struct ChannelMix
    {
        int32_t dry = 0;
        int32_t wet = 0;

        void add(int32_t drySample, int32_t wetSample, Sends sends)
        {
            if (sends.dry)
                dry += drySample;
            if (sends.wet)
                wet += wetSample;
        }

        int32_t finish(int32_t effect, int32_t effectGain, int32_t masterGain) const
        {
            return scale(saturate(saturate(dry) + scale(effect, effectGain)), masterGain);
        }
    };
}
