// PatchyLog.cpp — v0.0.919 (2026-09-29). See PatchyLog.h.

#include "PatchyLog.h"
#include <memory>
#include <mutex>

namespace PatchyLog
{
namespace
{
    constexpr juce::int64 kRollOverBytes = 2 * 1024 * 1024;
    constexpr int         kFlushMs       = 200;

    class AsyncFileLogger : public juce::Logger,
                            private juce::Thread
    {
    public:
        explicit AsyncFileLogger (const juce::File& f)
            : juce::Thread ("PatchyLog"), file (f)
        {
            file.getParentDirectory().createDirectory();
            if (file.getSize() > kRollOverBytes)
            {
                auto old = file.getSiblingFile ("Patchy.old.log");
                old.deleteFile();
                file.moveFileTo (old);
            }
            startThread (juce::Thread::Priority::low);
        }

        ~AsyncFileLogger() override
        {
            signalThreadShouldExit();
            notify();
            stopThread (2000);
            flush();   // whatever arrived after the thread's last pass
        }

        const juce::File& getFile() const { return file; }

    private:
        void logMessage (const juce::String& message) override
        {
            juce::Logger::outputDebugString (message);   // console, as before
            const juce::ScopedLock sl (queueLock);        // short: an append only
            queue.add (message);
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                wait (kFlushMs);
                flush();
            }
        }

        void flush()
        {
            juce::StringArray lines;
            {
                const juce::ScopedLock sl (queueLock);
                lines.swapWith (queue);
            }
            if (lines.isEmpty()) return;
            juce::FileOutputStream out (file);   // appends
            if (! out.openedOk()) return;
            for (auto& l : lines)
                out << l << juce::newLine;
        }

        juce::File             file;
        juce::CriticalSection  queueLock;
        juce::StringArray      queue;
    };

    std::mutex                       lock;
    int                              refs = 0;
    std::unique_ptr<AsyncFileLogger> logger;
}

Scope::Scope()
{
    std::lock_guard<std::mutex> g (lock);
    if (refs++ > 0) return;

    auto f = juce::FileLogger::getSystemLogFileFolder().getChildFile ("Patchy").getChildFile ("Patchy.log");
    logger = std::make_unique<AsyncFileLogger> (f);
    juce::Logger::setCurrentLogger (logger.get());
}

Scope::~Scope()
{
    std::lock_guard<std::mutex> g (lock);
    if (--refs > 0) return;
    if (juce::Logger::getCurrentLogger() == logger.get())
        juce::Logger::setCurrentLogger (nullptr);
    logger.reset();
}

juce::File getLogFile()
{
    std::lock_guard<std::mutex> g (lock);
    return logger != nullptr ? logger->getFile() : juce::File();
}
} // namespace PatchyLog
