#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace ps2x::audio
{
    class StereoPcmStream
    {
    public:
        enum class Capture { OnRead, OnStart, Ahead };
        static constexpr size_t kBlockFrames = 256u;
        static constexpr size_t kBlockBytes = kBlockFrames * 2u * sizeof(int16_t);
        using Channel = std::array<int16_t, kBlockFrames>;

        void start(std::span<const uint8_t> planar, Capture capture)
        {
            m_source = planar.first(planar.size() - planar.size() % kBlockBytes);
            m_next = 0u;
            m_capture = capture;
            m_staged = false;
            m_copy.clear();
            if (!hasBlock())
                return;
            if (capture == Capture::OnStart)
                m_copy.assign(m_source.begin(), m_source.end());
            else if (capture == Capture::Ahead)
                stage();
            // The currently playing block can finish while this transfer is queued.
        }

        bool needsBlock() const { return m_index == kBlockFrames; }
        bool hasBlock() const { return m_next < m_source.size(); }
        bool hasSource() const { return m_source.data() != nullptr; }
        size_t pendingBytes() const { return m_source.size() - m_next; }

        void loadBlock()
        {
            const uint8_t *block = m_staged ? m_pending.data() : sourceBlock();
            std::memcpy(m_left.data(), block, sizeof(Channel));
            std::memcpy(m_right.data(), block + sizeof(Channel), sizeof(Channel));
            m_next += kBlockBytes;
            m_index = 0u;
            m_staged = false;
            if (m_capture == Capture::Ahead && hasBlock())
                stage();
        }

        std::array<int32_t, 2> read()
        {
            const size_t index = m_index++;
            return {m_left[index], m_right[index]};
        }

        std::array<int32_t, 2> last() const { return {m_left.back(), m_right.back()}; }
        const Channel &left() const { return m_left; }
        const Channel &right() const { return m_right; }

    private:
        const uint8_t *sourceBlock() const { return (m_copy.empty() ? m_source.data() : m_copy.data()) + m_next; }
        void stage()
        {
            std::memcpy(m_pending.data(), sourceBlock(), kBlockBytes);
            m_staged = true;
        }

        std::span<const uint8_t> m_source;
        std::vector<uint8_t> m_copy;
        size_t m_next = 0u;
        size_t m_index = kBlockFrames;
        Capture m_capture = Capture::Ahead;
        bool m_staged = false;
        std::array<uint8_t, kBlockBytes> m_pending{};
        Channel m_left{};
        Channel m_right{};
    };
}
