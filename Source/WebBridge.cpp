#include "WebBridge.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"
#include "MidiMonitorNode.h"
#include "SerialPort.h"
#include <unordered_map>

#if HAS_BUNDLED_UI
  #include "BinaryData.h"
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static juce::String mimeFor (const juce::String& path)
{
    if (path.endsWith (".html")) return "text/html";
    if (path.endsWith (".js"))   return "application/javascript";
    if (path.endsWith (".css"))  return "text/css";
    if (path.endsWith (".svg"))  return "image/svg+xml";
    if (path.endsWith (".png"))  return "image/png";
    if (path.endsWith (".ico"))  return "image/x-icon";
    if (path.endsWith (".woff")) return "font/woff";
    if (path.endsWith (".woff2"))return "font/woff2";
    return "application/octet-stream";
}

static std::vector<std::byte> streamToVector (juce::InputStream& in)
{
    juce::MemoryOutputStream out;
    out.writeFromInputStream (in, -1);
    const auto& block = out.getMemoryBlock();
    std::vector<std::byte> result(block.getSize());
    std::memcpy(result.data(), block.getData(), block.getSize());
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Browser::pageAboutToLoad
//  Only allow our app URL and the resource provider root through.
// ─────────────────────────────────────────────────────────────────────────────

bool WebBridge::Browser::pageAboutToLoad (const juce::String& newURL)
{
    // Always allow the resource provider root (release mode)
    if (newURL == getResourceProviderRoot())
        return true;

    // Allow the Vite dev server (dev mode)
    if (owner.devServerUrl.isNotEmpty() && newURL.startsWith (owner.devServerUrl))
        return true;

    // Block everything else (prevents the webview navigating away)
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Resource provider (release mode)
//  Opens the bundled zip and serves files by their path inside the archive.
// ─────────────────────────────────────────────────────────────────────────────

std::optional<juce::WebBrowserComponent::Resource>
WebBridge::getResource (const juce::String& url)
{
    if (zipFile == nullptr)
        return std::nullopt;

    // JUCE may pass a full URL or just a path -- handle both
    juce::String path;
    if (url.startsWith ("http"))
        path = juce::URL (url).getSubPath().trimCharactersAtStart ("/");
    else
        path = url.trimCharactersAtStart ("/");

    if (path.isEmpty())
        path = "index.html";

    // Direct lookup first
    const juce::ZipFile::ZipEntry* entry = zipFile->getEntry (path);

    // Fallback: scan all entries matching by full name or trailing component
    if (entry == nullptr)
    {
        for (int i = 0; i < static_cast<int>(zipFile->getNumEntries()); ++i)
        {
            const juce::ZipFile::ZipEntry* e = zipFile->getEntry (i);
            if (e == nullptr) continue;
            if (e->filename == path || e->filename.endsWith ("/" + path))
            {
                entry = e;
                break;
            }
        }
    }

    if (entry == nullptr)
    {
        return std::nullopt;
    }

    auto stream = std::unique_ptr<juce::InputStream> (zipFile->createStreamForEntry (*entry));
    if (stream == nullptr)
        return std::nullopt;

    return juce::WebBrowserComponent::Resource {
        streamToVector (*stream),
        mimeFor (entry->filename)
    };
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────

WebBridge::WebBridge (GraphModel& model, PaxRegistry* reg,
                       std::function<void(const juce::String&, const juce::String&)> setMidiCb,
                       std::function<void(const juce::String&, const juce::String&)> setAudioCb,
                       std::function<std::vector<MidiMonitorBatch>()> monitorFn,
                       std::function<std::vector<AudioSnapshot>()> audioFn,
                       std::function<void()> trashFn,
                       std::function<std::vector<PortActivity>()> activityFn,
                       std::function<void(const juce::String&, uint8_t, uint8_t, uint8_t)> keyFn,
                       std::function<void(const juce::String&, const juce::String&)>          labelFn,
                       std::function<void(const juce::String&, int, float)>                      paramFn,
                       std::function<void()>                                                          newGraphFn,
                       std::function<void(const juce::String&)>                                       loadGraphFn)
    : graph (model),
      registry            (reg),
      drainMonitor        (std::move (monitorFn)),
      getAudioSnapshots   (std::move (audioFn)),
      getPortActivity     (std::move (activityFn)),
      clearGraphTrash     (std::move (trashFn)),
      onMidiKeyEvent      (std::move (keyFn)),
      onSetNodeLabel      (std::move (labelFn)),
      onSetPaxParameter   (std::move (paramFn)),
      onSetMidiDevice     (std::move (setMidiCb)),
      onSetAudioDevice    (std::move (setAudioCb)),
      onNewGraph          (std::move (newGraphFn)),
      onLoadGraph         (std::move (loadGraphFn))
{
#if HAS_BUNDLED_UI
    // ── Open the bundled zip ──────────────────────────────────────────────
    int         zipSize = 0;
    const char* zipData = BinaryData::getNamedResource ("ui_assets_zip", zipSize);

    jassert (zipData != nullptr && zipSize > 0);

    // Keep the raw memory alive in a MemoryInputStream that ZipFile reads from.
    zipStream = std::make_unique<juce::MemoryInputStream> (zipData, static_cast<size_t>(zipSize), false);
    zipFile   = std::make_unique<juce::ZipFile> (*zipStream);

    juce::Logger::writeToLog (
        juce::String ("WebBridge: zip loaded, ")
        + juce::String (zipFile->getNumEntries())
        + juce::String (" entries"));

    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled (true)
        .withResourceProvider (
            [this](const juce::String& url) { return getResource (url); },
            juce::URL (juce::WebBrowserComponent::getResourceProviderRoot()).getOrigin())
        .withEventListener ("graphMessage",
            [this](juce::var msg)
            {
                handleMessage (juce::JSON::toString (msg, true));
            })
#if JUCE_WINDOWS
        // Windows needs WebView2 with a writable user-data folder
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (
                    juce::File::getSpecialLocation (
                        juce::File::SpecialLocationType::tempDirectory)))
#endif
        ;

#else
    // ── Dev mode ──────────────────────────────────────────────────────────
    devServerUrl = "http://localhost:5173";

    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled (true)
        .withEventListener ("graphMessage",
            [this](juce::var msg)
            {
                handleMessage (juce::JSON::toString (msg, true));
            })
#if JUCE_WINDOWS
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (
                    juce::File::getSpecialLocation (
                        juce::File::SpecialLocationType::tempDirectory)))
#endif
        ;
#endif

    webView = std::make_unique<Browser> (*this, std::move (options));
    addAndMakeVisible (*webView);

    // NOTE: graph.onChange is set by PatchyProcessor and must not be overwritten here.
    // pushGraphToUI() is called from PatchyProcessor's onChange chain instead.
}

// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::resized()
{
    if (webView != nullptr)
        webView->setBounds (getLocalBounds());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helper — escape and push a JSON payload to a named bridge function
// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::pushToUI (const juce::String& bridgeFn, juce::String json)
{
    if (! connected || webView == nullptr) return;
    json = json.replace ("\\", "\\\\").replace ("`", "\\`");
    juce::String script;
    script << "if(window.__bridge&&window.__bridge." << bridgeFn << "){"
           << "window.__bridge." << bridgeFn << "(`" << json << "`);}";
    webView->evaluateJavascript (script, {});
}

void WebBridge::loadUI()
{
#if HAS_BUNDLED_UI
    // connected will be set true when UI sends "ready" message
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
#else
    webView->goToURL (devServerUrl);
#endif
}
