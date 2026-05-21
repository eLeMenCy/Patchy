#pragma once
#include "NodeProcessor.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>

/**
 * AudioMonitorBuffer  (owned by PatchyProcessor, survives graph rebuilds)
 *
 * Lock-free stereo ring buffer. Audio thread writes, message thread reads.
 * kRingSize = 8192 samples ≈ 170ms @ 48kHz — enough for any time window.
 */
struct AudioMonitorBuffer
{
    static constexpr int kRingSize = 8192;

    void push (const float* leftData, const float* rightData, int numSamples)
    {
        int wp = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
        {
            int idx  = (wp + i) % kRingSize;
            left[static_cast<size_t>(idx)]  = leftData  ? leftData[i]  : 0.0f;
            right[static_cast<size_t>(idx)] = rightData ? rightData[i] : 0.0f;
        }
        writePos.store ((wp + numSamples) % kRingSize, std::memory_order_release);
    }

    // Snapshot the most recent `count` samples into output vectors.
    // Called on message thread.
    void snapshot (int count, std::vector<float>& outLeft,
                                std::vector<float>& outRight) const
    {
        count = std::min (count, kRingSize);
        outLeft.resize  (static_cast<size_t>(count));
        outRight.resize (static_cast<size_t>(count));

        int wp  = writePos.load (std::memory_order_acquire);
        int start = (wp - count + kRingSize * 2) % kRingSize;

        for (int i = 0; i < count; ++i)
        {
            int idx      = (start + i) % kRingSize;
            outLeft[static_cast<size_t>(i)]   = left[static_cast<size_t>(idx)];
            outRight[static_cast<size_t>(i)]  = right[static_cast<size_t>(idx)];
        }
    }

    std::array<float, kRingSize> left  {};
    std::array<float, kRingSize> right {};
    std::atomic<int>             writePos { 0 };
};

/**
 * AudioMonitorNode  (nodeType 6)
 *
 * 1 Audio In + 1 Audio Out (stereo pass-through + capture).
 * Holds a raw pointer to an AudioMonitorBuffer owned by PatchyProcessor.
 */
class AudioMonitorNode : public NodeProcessor
{
public:
    AudioMonitorNode (const juce::String& nodeId, AudioMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Audio), buffer (sharedBuffer) {}

    void process (int numSamples) override
    {
        // Pass audio through
        for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
            outputAudio.copyFrom (ch, 0, inputAudio, ch, 0, numSamples);

        if (buffer == nullptr) return;

        const float* L = inputAudio.getNumChannels() > 0
                           ? inputAudio.getReadPointer (0) : nullptr;
        const float* R = inputAudio.getNumChannels() > 1
                           ? inputAudio.getReadPointer (1) : nullptr;
        buffer->push (L, R, numSamples);
    }

private:
    AudioMonitorBuffer* buffer = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioMonitorNode)
};
