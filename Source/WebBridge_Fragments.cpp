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

// ── Fragment export ───────────────────────────────────────────────────────────
void WebBridge::showExportDialog (const juce::StringArray& selectedNodeIds,
                                   const juce::String& suggestedName)
{
    auto startDir = currentFile.existsAsFile()
                    ? currentFile.getParentDirectory()
                    : (lastOpenDir.isDirectory()
                       ? lastOpenDir
                       : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

    auto defaultFile = startDir.getChildFile (suggestedName + ".patchy");

    auto chooser = std::make_shared<juce::FileChooser> (
        "Export Fragment", defaultFile, "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser, selectedNodeIds] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result == juce::File{}) return;

            auto f = result.withFileExtension ("patchy");
            lastOpenDir = f.getParentDirectory();

            // Build fragment: filter graph to selected nodes only,
            // keeping connections that are entirely within the selection.
            auto fullGraph = graph.toVar();
            auto* fullObj  = fullGraph.getDynamicObject();
            if (fullObj == nullptr) return;

            // Collect selected nodes
            juce::Array<juce::var> fragNodes;
            auto* allNodes = fullObj->getProperty ("nodes").getArray();
            if (allNodes)
                for (auto& n : *allNodes)
                    if (auto* nObj = n.getDynamicObject())
                        if (selectedNodeIds.contains (nObj->getProperty ("id").toString()))
                            fragNodes.add (n);

            // Collect internal connections only (both ends in selection)
            juce::Array<juce::var> fragConns;
            auto* allConns = fullObj->getProperty ("connections").getArray();
            if (allConns)
                for (auto& c : *allConns)
                    if (auto* cObj = c.getDynamicObject())
                        if (selectedNodeIds.contains (cObj->getProperty ("sourceNodeId").toString()) &&
                            selectedNodeIds.contains (cObj->getProperty ("targetNodeId").toString()))
                            fragConns.add (c);

            // Compute bounding box for ghost sizing on import
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (auto& n : fragNodes)
                if (auto* nObj = n.getDynamicObject())
                {
                    float x = (float) (double) nObj->getProperty ("x");
                    float y = (float) (double) nObj->getProperty ("y");
                    minX = std::min (minX, x); minY = std::min (minY, y);
                    maxX = std::max (maxX, x); maxY = std::max (maxY, y);
                }
            // Add approximate node dimensions
            float fragW = (maxX - minX) + 160.f;
            float fragH = (maxY - minY) + 80.f;

            // Normalise positions relative to top-left of bounding box
            for (auto& n : fragNodes)
                if (auto* nObj = n.getDynamicObject())
                {
                    nObj->setProperty ("x", (double) nObj->getProperty ("x") - minX);
                    nObj->setProperty ("y", (double) nObj->getProperty ("y") - minY);
                }

            auto fragObj = std::make_unique<juce::DynamicObject>();
            fragObj->setProperty ("nodes",       juce::var (fragNodes));
            fragObj->setProperty ("connections", juce::var (fragConns));
            fragObj->setProperty ("width",       (double) fragW);
            fragObj->setProperty ("height",      (double) fragH);

            auto json = juce::JSON::toString (juce::var (fragObj.release()), true);
            f.replaceWithText (json);
        });
}

// ── Fragment import ───────────────────────────────────────────────────────────
void WebBridge::showImportDialog()
{
    auto startDir = currentFile.existsAsFile()
                    ? currentFile.getParentDirectory()
                    : (lastOpenDir.isDirectory()
                       ? lastOpenDir
                       : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

    auto chooser = std::make_shared<juce::FileChooser> (
        "Import Fragment", startDir, "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result == juce::File{} || ! result.existsAsFile()) return;

            lastOpenDir = result.getParentDirectory();
            auto json = result.loadFileAsString();
            if (json.isEmpty()) return;

            juce::var parsed;
            if (juce::JSON::parse (json, parsed).failed()) return;

            // Remap all IDs to fresh UUIDs before sending to React
            auto remapped = remapFragmentIds (parsed, {});
            pushToUI ("onFragmentReady", juce::JSON::toString (remapped, false));
        });
}

// ── ID remapping — produces a fragment with fresh UUIDs ──────────────────────
juce::var WebBridge::remapFragmentIds (const juce::var& fragment,
                                        const juce::StringArray& /*selectedNodeIds*/)
{
    auto* srcObj = fragment.getDynamicObject();
    if (srcObj == nullptr) return fragment;

    // Build old→new ID map for nodes
    std::unordered_map<juce::String, juce::String> idMap;

    auto* srcNodes = srcObj->getProperty ("nodes").getArray();
    juce::Array<juce::var> newNodes;

    if (srcNodes)
    {
        for (auto& n : *srcNodes)
        {
            auto* nObj = n.getDynamicObject();
            if (! nObj) continue;

            juce::String oldId = nObj->getProperty ("id").toString();
            juce::String newId = juce::Uuid().toString();
            idMap[oldId] = newId;

            // Deep-copy node and remap its id + port ids
            auto newNode = std::make_unique<juce::DynamicObject>();
            auto props = nObj->getProperties();
            for (auto& prop : props)
                newNode->setProperty (prop.name, prop.value);

            newNode->setProperty ("id", newId);

            // Remap port IDs (format: oldNodeId_PortLabel_direction)
            if (auto* ports = nObj->getProperty ("ports").getArray())
            {
                juce::Array<juce::var> newPorts;
                for (auto& p : *ports)
                {
                    if (auto* pObj = p.getDynamicObject())
                    {
                        auto newPort = std::make_unique<juce::DynamicObject>();
                        auto pProps = pObj->getProperties();
                        for (auto& pp : pProps)
                            newPort->setProperty (pp.name, pp.value);

                        juce::String oldPortId = pObj->getProperty ("id").toString();
                        // Replace the node-id prefix in the port id
                        if (oldPortId.startsWith (oldId))
                            newPort->setProperty ("id", newId + oldPortId.substring (oldId.length()));

                        newPorts.add (juce::var (newPort.release()));
                    }
                }
                newNode->setProperty ("ports", juce::var (newPorts));
            }

            newNodes.add (juce::var (newNode.release()));
        }
    }

    // Remap connection IDs and node/port references
    auto* srcConns = srcObj->getProperty ("connections").getArray();
    juce::Array<juce::var> newConns;

    if (srcConns)
    {
        for (auto& c : *srcConns)
        {
            auto* cObj = c.getDynamicObject();
            if (! cObj) continue;

            juce::String oldSrcNode = cObj->getProperty ("sourceNodeId").toString();
            juce::String oldDstNode = cObj->getProperty ("targetNodeId").toString();

            // Skip connections that cross the boundary (node not in map)
            if (idMap.find (oldSrcNode) == idMap.end()) continue;
            if (idMap.find (oldDstNode) == idMap.end()) continue;

            auto newConn = std::make_unique<juce::DynamicObject>();
            newConn->setProperty ("id",           juce::Uuid().toString());
            newConn->setProperty ("sourceNodeId", idMap[oldSrcNode]);
            newConn->setProperty ("targetNodeId", idMap[oldDstNode]);

            // Remap port IDs in connections
            juce::String oldSrcPort = cObj->getProperty ("sourcePortId").toString();
            juce::String oldDstPort = cObj->getProperty ("targetPortId").toString();
            for (auto& [oldId, newId] : idMap)
            {
                if (oldSrcPort.startsWith (oldId))
                    oldSrcPort = newId + oldSrcPort.substring (oldId.length());
                if (oldDstPort.startsWith (oldId))
                    oldDstPort = newId + oldDstPort.substring (oldId.length());
            }
            newConn->setProperty ("sourcePortId", oldSrcPort);
            newConn->setProperty ("targetPortId", oldDstPort);

            newConns.add (juce::var (newConn.release()));
        }
    }

    auto result = std::make_unique<juce::DynamicObject>();
    result->setProperty ("nodes",       juce::var (newNodes));
    result->setProperty ("connections", juce::var (newConns));
    result->setProperty ("width",       srcObj->getProperty ("width"));
    result->setProperty ("height",      srcObj->getProperty ("height"));

    return juce::var (result.release());
}
