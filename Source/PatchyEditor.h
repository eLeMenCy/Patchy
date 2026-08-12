#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PatchyProcessor.h"
#include "WebBridge.h"

/**
 * PatchyEditor
 *
 * The AudioProcessorEditor that hosts the WebBridge (and therefore
 * the entire React UI). It is the direct equivalent of MainComponent
 * in the standalone app.
 *
 * The editor is intentionally thin — all UI logic lives in React,
 * all audio logic lives in PatchyProcessor / ProcessingGraph.
 */
class PatchyEditor : public juce::AudioProcessorEditor,
                       public juce::KeyListener
{
public:
    explicit PatchyEditor (PatchyProcessor& p);
    ~PatchyEditor() override = default;

    void paint                  (juce::Graphics&) override;
    void resized                ()                override;
    void parentHierarchyChanged ()                override;
    using juce::Component::keyPressed;  // prevent hiding Component::keyPressed
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;  // juce::KeyListener

    WebBridge& getBridge() { return bridge; }

private:
    WebBridge bridge;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchyEditor)
};
