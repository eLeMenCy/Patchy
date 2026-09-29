#include "DmxDeviceNodes.h"
#include "ProcessingGraph.h"
#include <cstring>

bool DmxDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph, ProcessingGraph* oldGraph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* in = graph.findDmxInNode (nodeId))
    {
        // Real fix, 2026-09-01 — see DmxInDeviceNode's own
        // transferOrConfigure() for the full story. oldGraph is the most
        // recent previous graph (whichever node instance genuinely has
        // this exact device already open, if any) — reusing that
        // connection instead of closing and reopening it on every
        // single graph rebuild, anywhere, is what actually fixes the
        // real, graph-wide sluggishness this was built to address.
        auto* oldIn = oldGraph != nullptr ? oldGraph->findDmxInNode (nodeId) : nullptr;
        in->transferOrConfigure (s.devicePath, oldIn);
        return true;
    }
    if (auto* out = graph.findDmxOutNode (nodeId))
    {
        auto* oldOut = oldGraph != nullptr ? oldGraph->findDmxOutNode (nodeId) : nullptr;
        out->transferOrConfigure (s.devicePath, s.universe, oldOut);
        return true;
    }
    return false;
}

void DmxDeviceManager::applyAllSettings (ProcessingGraph& graph, ProcessingGraph* oldGraph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph, oldGraph);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DmxSharedPort — v0.0.916 (2026-09-28). See the class comment in the header.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
    std::mutex                                                   dmxRegistryLock;
    std::map<juce::String, std::weak_ptr<DmxSharedPort>>        dmxRegistry;
}

std::shared_ptr<DmxSharedPort> DmxSharedPort::acquire (const juce::String& devicePath, OpenResult& result)
{
    std::lock_guard<std::mutex> g (dmxRegistryLock);

    if (auto existing = dmxRegistry[devicePath].lock())
    {
        result = OpenResult::ok;
        return existing;
    }

    std::shared_ptr<DmxSharedPort> port (new DmxSharedPort (devicePath));
    if (! port->serial.open (devicePath.toStdString(), 57600, 8, 'N', 2))
    {
        result = port->serial.lastOpenWasBusy() ? OpenResult::inUse : OpenResult::failed;
        dmxRegistry.erase (devicePath);
        return nullptr;
    }

    port->mk2 = EnttecProCodec::detectIsMk2 (port->serial, &port->leftover);
    juce::Logger::writeToLog ("DmxSharedPort: opened " + devicePath + " (exclusive), device is "
                              + juce::String (port->mk2 ? "Pro Mk2" : "Pro"));
    dmxRegistry[devicePath] = port;
    result = OpenResult::ok;
    return port;
}

DmxSharedPort::DmxSharedPort (const juce::String& path)
    : juce::Thread ("DmxPort_" + path.fromLastOccurrenceOf ("/", false, false)),
      devicePath (path)
{}

DmxSharedPort::~DmxSharedPort()
{
    signalThreadShouldExit();
    stopThread (1000);   // read() uses a 5 ms timeout, so this returns promptly
    serial.close();
    juce::Logger::writeToLog ("DmxSharedPort: closed " + devicePath);
}

int DmxSharedPort::writePacket (const uint8_t* data, int len)
{
    std::lock_guard<std::mutex> g (writeLock);
    return serial.write (data, len);
}

void DmxSharedPort::addIn (InListener* listener)
{
    bool first = false;
    {
        std::lock_guard<std::mutex> g (inLock);
        if (std::find (ins.begin(), ins.end(), listener) != ins.end()) return;
        ins.push_back (listener);
        inCount.store ((int) ins.size());
        first = (ins.size() == 1);
    }
    if (! first || isThreadRunning()) return;

    // Moved unchanged from DmxInDeviceNode::openPort() — see the history
    // there (2026-08-31, 3rd pass): the classic Pro needs this to switch its
    // half-duplex port to input, then a short settling delay.
    uint8_t buf[16];
    const int len = EnttecProCodec::buildEnableReceivePacket (buf, sizeof (buf));
    if (len > 0)
    {
        const int written = writePacket (buf, len);
        juce::Logger::writeToLog ("DmxSharedPort: enable-receive sent on " + devicePath + " ("
                                  + juce::String (written) + " of " + juce::String (len) + " bytes)");
    }
    juce::Thread::sleep (100);
    startThread (juce::Thread::Priority::normal);
}

void DmxSharedPort::removeIn (InListener* listener)
{
    bool none = false;
    {
        std::lock_guard<std::mutex> g (inLock);   // waits for a running dispatch
        ins.erase (std::remove (ins.begin(), ins.end(), listener), ins.end());
        inCount.store ((int) ins.size());
        none = ins.empty();
    }
    if (none)
        stopThread (1000);   // outside inLock: run() takes it to dispatch
}

void DmxSharedPort::run()
{
    // Moved from DmxInDeviceNode::run() (v0.0.916). Change detection moved
    // to each In node, so every frame is dispatched.
    static constexpr int kBufSize = 600;
    std::array<uint8_t, kBufSize> rxBuf {};
    std::array<uint8_t, 512>      dmxOut {};
    int rxLen = 0;

    // Seed with what detectIsMk2() read but didn't consume (first start only).
    if (! leftover.empty())
    {
        const int seedLen = std::min ((int) leftover.size(), kBufSize);
        std::memcpy (rxBuf.data(), leftover.data(), (size_t) seedLen);
        rxLen = seedLen;
        leftover.clear();
    }

    while (! threadShouldExit())
    {
        if (! serial.isOpen()) break;

        uint8_t tmp[64];
        const int n = serial.read (tmp, sizeof (tmp), 5);  // 5 ms timeout — DMX frame = 22 ms
        if (n <= 0) continue;

        {
            std::lock_guard<std::mutex> g (inLock);
            for (auto* l : ins) l->dmxBytesReceived (n);
        }

        const int copyLen = std::min (n, kBufSize - rxLen);
        if (copyLen > 0)
        {
            std::memcpy (rxBuf.data() + rxLen, tmp, (size_t) copyLen);
            rxLen += copyLen;
        }

        const int channels = EnttecProCodec::parseRxPacket (rxBuf.data(), rxLen, dmxOut.data());
        if (channels > 0)
        {
            std::lock_guard<std::mutex> g (inLock);
            for (auto* l : ins) l->dmxFrameReceived (dmxOut.data());
            rxLen = 0;   // consumed — reset buffer
        }
        else if (rxLen >= kBufSize)
        {
            rxLen = 0;   // overflow — discard and resync
        }
    }
}
