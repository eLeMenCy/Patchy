#pragma once
#include <juce_core/juce_core.h>
#include <map>
#include <mutex>

/**
 * StartupFadeRegistry.h — targeted AudioIn startup fade, 2026-09-24.
 *
 * App-wide list ("blacklist") of audio input devices known to emit a pop
 * shortly after their stream starts (first confirmed: Behringer FCA1616,
 * pop ~1 s after stream start). For each listed device it stores the mute
 * duration in ms. AudioInDeviceNode::openDevice() looks the device up on
 * every FRESH open: listed → arm the mute-then-ramp fade with that
 * duration; not listed → audio passes immediately.
 *
 * Deliberately NOT per-graph and NOT per-node: the pop is a property of the
 * hardware, so ticking "Startup fade" once on any AudioIn node using a
 * device applies to that device in every graph, from its next fresh open.
 * The frontend has no per-node state for this at all — a node's toggle is
 * simply derived from whether its selected device is listed here.
 *
 * Persisted as a small JSON object { "deviceName": muteMs, ... } in
 * ~/Library/Application Support/Patchy/StartupFadeDevices.json (the same
 * userApplicationDataDirectory the standalone's own Patchy.settings lives
 * in), so it works identically in Standalone and plugin builds. Loaded
 * lazily on first use; saved on every change. All access is guarded by one
 * mutex — only ever called from the message thread (openDevice, the
 * WebBridge dispatch), never from an audio callback.
 */
namespace StartupFadeRegistry
{
    inline constexpr int kDefaultMuteMs = 1250;
    inline constexpr int kMinMuteMs     = 0;
    inline constexpr int kMaxMuteMs     = 5000;

    namespace detail
    {
        struct State
        {
            std::mutex                    lock;
            std::map<juce::String, int>   devices;
            bool                          loaded = false;
        };

        inline State& state()
        {
            static State s;
            return s;
        }

        inline juce::File file()
        {
            return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("Patchy")
                       .getChildFile ("StartupFadeDevices.json");
        }

        // Caller must hold state().lock.
        inline void loadIfNeeded (State& s)
        {
            if (s.loaded) return;
            s.loaded = true;
            const auto f = file();
            if (! f.existsAsFile()) return;
            const auto parsed = juce::JSON::parse (f.loadFileAsString());
            if (auto* obj = parsed.getDynamicObject())
                for (auto& prop : obj->getProperties())
                    s.devices[prop.name.toString()] =
                        juce::jlimit (kMinMuteMs, kMaxMuteMs, (int) prop.value);
        }

        // Caller must hold state().lock.
        inline void save (const State& s)
        {
            auto* obj = new juce::DynamicObject();
            for (auto& [name, ms] : s.devices)
                obj->setProperty (juce::Identifier (name), ms);
            const auto f = file();
            f.getParentDirectory().createDirectory();
            f.replaceWithText (juce::JSON::toString (juce::var (obj), true));
        }
    }

    /** Mute duration in ms for this device, or -1 if it isn't listed. */
    inline int getMuteMs (const juce::String& deviceName)
    {
        auto& s = detail::state();
        std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        auto it = s.devices.find (deviceName);
        return it != s.devices.end() ? it->second : -1;
    }

    /** Add/update (enabled) or remove (!enabled) a device, then persist. */
    inline void setDevice (const juce::String& deviceName, bool enabled, int muteMs)
    {
        if (deviceName.isEmpty() || deviceName == "DAW") return;
        auto& s = detail::state();
        std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        if (enabled) s.devices[deviceName] = juce::jlimit (kMinMuteMs, kMaxMuteMs, muteMs);
        else         s.devices.erase (deviceName);
        detail::save (s);
    }

    /** Whole list as { "deviceName": muteMs, ... } for the frontend. */
    inline juce::var toVar()
    {
        auto& s = detail::state();
        std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        auto* obj = new juce::DynamicObject();
        for (auto& [name, ms] : s.devices)
            obj->setProperty (juce::Identifier (name), ms);
        return juce::var (obj);
    }
}
