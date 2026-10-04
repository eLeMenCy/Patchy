#pragma once
#include <juce_core/juce_core.h>
#include <mutex>

/**
 * AppSettings.h — v0.0.923 (2026-10-04)
 *
 * Patchy-wide (not per project) settings, persisted as JSON in
 * ~/Library/Application Support/Patchy/AppSettings.json — same pattern as
 * StartupFadeRegistry.h. Shared by the Standalone and the plug-ins.
 *
 *  - recentFiles: the last kMaxRecent .patchy files opened or saved, newest
 *    first (File menu → Open Recent).
 *  - reopenLastProject: Standalone only — at launch, reopen the project that
 *    was open when Patchy was last closed (lastSessionFile). -1 = never asked yet (the Standalone asks once, on its first
 *    launch), 0 = no, 1 = yes. Changeable in Preferences.
 *
 *  - lastSessionFile: that project — set when a patch is opened or saved,
 *    cleared by File → New and when it's found missing at launch, so quitting
 *    on an empty graph means a blank start next time (user, 2026-10-04: it
 *    used to reopen the newest recent file instead).
 *
 * Message thread in practice; guarded by a mutex anyway.
 */
namespace AppSettings
{
    inline constexpr int kMaxRecent = 10;   // user: "if it feels too much, easy to reduce"

    namespace detail
    {
        struct State
        {
            std::mutex        lock;
            juce::StringArray recent;
            int               reopen = -1;
            juce::String      lastSession;
            bool              loaded = false;
        };
        inline State& state() { static State s; return s; }

        inline juce::File file()
        {
            return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("Patchy").getChildFile ("AppSettings.json");
        }

        // Caller holds the lock.
        inline void loadIfNeeded (State& s)
        {
            if (s.loaded) return;
            s.loaded = true;
            const auto f = file();
            if (! f.existsAsFile()) return;
            const auto v = juce::JSON::parse (f.loadFileAsString());
            if (auto* arr = v["recentFiles"].getArray())
                for (auto& p : *arr)
                    if (p.toString().isNotEmpty() && s.recent.size() < kMaxRecent)
                        s.recent.add (p.toString());
            const auto r = v["reopenLastProject"];
            s.reopen = r.isVoid() || r.isUndefined() ? -1 : ((bool) r ? 1 : 0);
            s.lastSession = v["lastSessionFile"].toString();
        }

        // Caller holds the lock.
        inline void save (const State& s)
        {
            auto* obj = new juce::DynamicObject();
            juce::Array<juce::var> arr;
            for (auto& p : s.recent) arr.add (p);
            obj->setProperty ("recentFiles", arr);
            if (s.reopen >= 0) obj->setProperty ("reopenLastProject", s.reopen == 1);
            obj->setProperty ("lastSessionFile", s.lastSession);
            const auto f = file();
            f.getParentDirectory().createDirectory();
            f.replaceWithText (juce::JSON::toString (juce::var (obj), true));
        }
    }

    inline juce::StringArray getRecentFiles()
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        return s.recent;
    }

    /** Moves (or adds) the file to the top of the list. */
    inline void addRecentFile (const juce::File& f)
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        const auto path = f.getFullPathName();
        s.recent.removeString (path);
        s.recent.insert (0, path);
        while (s.recent.size() > kMaxRecent) s.recent.remove (s.recent.size() - 1);
        detail::save (s);
    }

    inline void removeRecentFile (const juce::String& path)
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        s.recent.removeString (path);
        detail::save (s);
    }

    inline void clearRecentFiles()
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        s.recent.clear();
        detail::save (s);
    }

    /** The project open when Patchy was last closed ("" = none). */
    inline juce::String getLastSessionFile()
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        return s.lastSession;
    }

    inline void setLastSessionFile (const juce::String& path)
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        if (s.lastSession == path) return;
        s.lastSession = path;
        detail::save (s);
    }

    /** -1 never asked, 0 no, 1 yes. */
    inline int getReopenLastProject()
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        return s.reopen;
    }

    inline void setReopenLastProject (bool on)
    {
        auto& s = detail::state(); std::lock_guard<std::mutex> g (s.lock);
        detail::loadIfNeeded (s);
        s.reopen = on ? 1 : 0;
        detail::save (s);
    }
}
