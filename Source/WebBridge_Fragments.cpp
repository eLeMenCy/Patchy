#include "WebBridge.h"
#include "AppSettings.h"
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

            auto fragment = buildFragment (selectedNodeIds);
            if (fragment.isVoid()) return;
            auto json = juce::JSON::toString (fragment, true);
            f.replaceWithText (json);
        });
}

// ── Fragment building (v0.0.938: also carries whole wraps) ───────────────────
// The selected nodes, the wires between them, and every wrap whose nodes are
// all in the selection. Positions are made relative: to the selection's
// top-left, or — `originWrapId`, for the wrap library — to that wrap's box,
// so a dropped wrap's box lands exactly where it is dropped.
juce::var WebBridge::buildFragment (const juce::StringArray& selectedNodeIds, const juce::String& originWrapId)
{
    auto fullGraph = graph.toVar();
    auto* fullObj  = fullGraph.getDynamicObject();
    if (fullObj == nullptr) return {};

    juce::Array<juce::var> fragNodes;
    if (auto* allNodes = fullObj->getProperty ("nodes").getArray())
        for (auto& n : *allNodes)
            if (auto* nObj = n.getDynamicObject())
                if (selectedNodeIds.contains (nObj->getProperty ("id").toString()))
                    fragNodes.add (n);

    juce::Array<juce::var> fragConns;
    if (auto* allConns = fullObj->getProperty ("connections").getArray())
        for (auto& c : *allConns)
            if (auto* cObj = c.getDynamicObject())
                if (selectedNodeIds.contains (cObj->getProperty ("sourceNodeId").toString()) &&
                    selectedNodeIds.contains (cObj->getProperty ("targetNodeId").toString()))
                    fragConns.add (c);

    juce::Array<juce::var> fragWraps;
    if (auto* allWraps = GraphModel::wrapsOf (*fullObj).getArray())
        for (auto& w : *allWraps)
            if (auto* wObj = w.getDynamicObject())
            {
                bool all = true;
                if (auto* ids = wObj->getProperty ("nodeIds").getArray())
                    for (auto& id : *ids) all = all && selectedNodeIds.contains (id.toString());
                if (all) fragWraps.add (w);
            }

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (auto& n : fragNodes)
        if (auto* nObj = n.getDynamicObject())
        {
            const float x = (float) (double) nObj->getProperty ("x"), y = (float) (double) nObj->getProperty ("y");
            minX = std::min (minX, x); minY = std::min (minY, y);
            maxX = std::max (maxX, x); maxY = std::max (maxY, y);
        }
    if (originWrapId.isNotEmpty())
        if (auto* ow = graph.findWrap (originWrapId)) { minX = ow->x; minY = ow->y; }

    for (auto& n : fragNodes)
        if (auto* nObj = n.getDynamicObject())
        {
            nObj->setProperty ("x", (double) nObj->getProperty ("x") - minX);
            nObj->setProperty ("y", (double) nObj->getProperty ("y") - minY);
        }
    for (auto& w : fragWraps)
        if (auto* wObj = w.getDynamicObject())
        {
            wObj->setProperty ("x", (double) wObj->getProperty ("x") - minX);
            wObj->setProperty ("y", (double) wObj->getProperty ("y") - minY);
            if (originWrapId.isNotEmpty()) wObj->setProperty ("open", false);   // a library wrap lands folded
        }

    auto fragObj = std::make_unique<juce::DynamicObject>();
    fragObj->setProperty ("nodes",       juce::var (fragNodes));
    fragObj->setProperty ("connections", juce::var (fragConns));
    fragObj->setProperty ("wraps",       juce::var (fragWraps));
    fragObj->setProperty ("width",       (double) ((maxX - minX) + 160.f));
    fragObj->setProperty ("height",      (double) ((maxY - minY) + 80.f));
    return juce::var (fragObj.release());
}

// ── Placing a fragment in the graph (v0.0.938) ───────────────────────────────
// Shared by fragment import and the wrap library: nodes (shifted by dx/dy),
// the wires between them, then their wraps — one undo step, one rebuild.
// v0.0.941 — nodes of a Pax that isn't installed are placed too, as ghosts
// (the processing graph skips them: no processor), with their wires, pins and
// wrap membership; a warning names the missing Pax.
void WebBridge::commitFragment (const juce::DynamicObject& frag, float dx, float dy)
{
    auto* nodesArr = frag.getProperty ("nodes").getArray();
    if (nodesArr == nullptr) return;

    juce::StringArray missing;

    pendingSettingsSnapshot = juce::var(); pendingSettingsNodeId.clear();
    graph.pushSnapshot();
    graph.suspendNotifications();

    for (auto& n : *nodesArr)
    {
        auto* nObj = n.getDynamicObject();
        if (! nObj) continue;

        juce::String savedId  = nObj->getProperty ("id").toString();
        int          nodeType = (int) nObj->getProperty ("nodeType");
        float        x        = (float) (double) nObj->getProperty ("x") + dx;
        float        y        = (float) (double) nObj->getProperty ("y") + dy;
        juce::String paxName  = nObj->getProperty ("paxName").toString();
        if (paxName.isNotEmpty() && ! isPaxInstalled (paxName))
            missing.addIfNotAlreadyThere (paxName);   // placed anyway, as a ghost
        PaxPortSpec spec;
        spec.audioIn  = (int) nObj->getProperty ("audioInputs");
        spec.audioOut = (int) nObj->getProperty ("audioOutputs");
        spec.midiIn   = (int) nObj->getProperty ("midiInputs");
        spec.midiOut  = (int) nObj->getProperty ("midiOutputs");
        spec.valueIn  = (int) nObj->getProperty ("valueInputs");
        spec.valueOut = (int) nObj->getProperty ("valueOutputs");
        if (auto* arr = nObj->getProperty ("valueInputTypes").getArray())
            for (auto& tag : *arr) spec.valueInTypes.push_back (parseValueTypeTag (tag.toString()));
        if (auto* arr = nObj->getProperty ("valueOutputTypes").getArray())
            for (auto& tag : *arr) spec.valueOutTypes.push_back (parseValueTypeTag (tag.toString()));

        auto& nd = graph.restoreNode (savedId, nodeType, x, y, paxName, spec);
        juce::String devId = nObj->getProperty ("selectedDeviceId").toString();
        if (devId.isNotEmpty()) nd.selectedDeviceId = devId;
        juce::String settings = nObj->getProperty ("settingsJson").toString();
        if (settings.isNotEmpty()) nd.settingsJson = settings;
        juce::String label = nObj->getProperty ("label").toString();
        if (label.isNotEmpty()) nd.label = label;
        nd.customName = nObj->getProperty ("customName").toString();
        nd.disabled   = (bool) nObj->getProperty ("disabled");
    }

    if (auto* connsArr = frag.getProperty ("connections").getArray())
        for (auto& c : *connsArr)
            if (auto* cObj = c.getDynamicObject())
            {
                const auto sn = cObj->getProperty ("sourceNodeId").toString(), tn = cObj->getProperty ("targetNodeId").toString();
                graph.addConnection (sn, cObj->getProperty ("sourcePortId").toString(),
                                     tn, cObj->getProperty ("targetPortId").toString());
            }

    if (auto* wrapsArr = frag.getProperty ("wraps").getArray())
        for (auto& w : *wrapsArr)
            if (auto* wObj = w.getDynamicObject())
            {
                juce::StringArray ids, ports;
                if (auto* a = wObj->getProperty ("nodeIds").getArray())
                    for (auto& v : *a) if (graph.findNode (v.toString()) != nullptr) ids.add (v.toString());
                if (ids.isEmpty()) continue;
                if (auto* a = wObj->getProperty ("ports").getArray())
                    for (auto& v : *a) if (ids.contains (v.toString().upToFirstOccurrenceOf ("_", false, false))) ports.add (v.toString());
                auto& nw = graph.addWrap (wObj->getProperty ("name").toString(), ids, ports,
                                          (float) (double) wObj->getProperty ("x") + dx,
                                          (float) (double) wObj->getProperty ("y") + dy);
                nw.open = (bool) wObj->getProperty ("open");
                if (auto* a = wObj->getProperty ("pins").getArray())
                    for (auto& pv : *a)
                        if (auto* q = pv.getDynamicObject())
                            if (ids.contains (q->getProperty ("nodeId").toString()))
                                nw.pins.push_back ({ q->getProperty ("nodeId").toString(), (int) q->getProperty ("index"),
                                                     q->getProperty ("label").toString() });
            }

    graph.resumeNotifications();   // one rebuild, pushes the graph

    warnMissingPax (missing);
}

// v0.0.941 — ghost nodes
bool WebBridge::isPaxInstalled (const juce::String& name) const
{
    if (registry == nullptr) return true;
    for (const auto& e : registry->getEntries()) if (e.name == name) return true;
    return false;
}

void WebBridge::warnMissingPax (const juce::StringArray& missing)
{
    if (missing.isEmpty()) return;
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
        .withIconType (juce::MessageBoxIconType::WarningIcon)
        .withTitle ("Missing Pax")
        .withMessage ("These Pax aren't installed, so their nodes are shown as ghosts (greyed, passing nothing):\n\n  "
                      + missing.joinIntoString ("\n  ")
                      + "\n\nTheir settings and wires are kept. Install them in the Pax folder and restart Patchy to bring them back.")
        .withButton ("OK"), nullptr);
}

void WebBridge::warnMissingPaxInGraph()
{
    juce::StringArray missing;
    for (const auto& n : graph.getNodes())
        if (n.paxName.isNotEmpty() && ! isPaxInstalled (n.paxName))
            missing.addIfNotAlreadyThere (n.paxName);
    warnMissingPax (missing);
}

// ── Wrap library (v0.0.938) ──────────────────────────────────────────────────
// One small .patchy per saved wrap (a fragment holding exactly that wrap) in
// ~/Documents/Patchy/Wraps. The sidebar lists them; dropping one places an
// independent copy, folded, at the drop point.
juce::File WebBridge::defaultWrapLibraryDir()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Patchy").getChildFile ("Wraps");
}

// v0.0.939 — the folder chosen in Preferences, else the default. A chosen
// folder that isn't there (unplugged drive, renamed…) is NOT re-created: the
// default is used meanwhile, and the setting kept for when it's back.
juce::File WebBridge::wrapLibraryDir() const
{
    const auto chosen = AppSettings::getWrapLibraryDir();
    if (chosen.isNotEmpty() && juce::File::isAbsolutePath (chosen) && juce::File (chosen).isDirectory())
        return juce::File (chosen);
    auto dir = defaultWrapLibraryDir();
    dir.createDirectory();
    return dir;
}

void WebBridge::pushWrapLibrary()
{
    juce::Array<juce::var> items;
    auto files = wrapLibraryDir().findChildFiles (juce::File::findFiles, false, "*.patchy");
    files.sort();
    for (const auto& f : files)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("file", f.getFullPathName());
        o->setProperty ("name", f.getFileNameWithoutExtension());
        juce::var parsed;
        if (juce::JSON::parse (f.loadFileAsString(), parsed).wasOk())
            if (auto* nodes = parsed.getProperty ("nodes", {}).getArray()) o->setProperty ("nodes", nodes->size());
        items.add (o);
    }
    auto* payload = new juce::DynamicObject();   // v0.0.939 — + the folder (Preferences)
    payload->setProperty ("folder",    wrapLibraryDir().getFullPathName());
    payload->setProperty ("isDefault", wrapLibraryDir() == defaultWrapLibraryDir());
    // a chosen folder that can't be reached right now (the default is in use)
    const auto chosen = AppSettings::getWrapLibraryDir();
    payload->setProperty ("unreachable", chosen.isNotEmpty() && ! (juce::File::isAbsolutePath (chosen) && juce::File (chosen).isDirectory())
                                         ? chosen : juce::String());
    payload->setProperty ("items",     items);
    pushToUI ("onWrapLibrary", juce::JSON::toString (juce::var (payload), true));
}

void WebBridge::handleWrapLibrary (const juce::String& type, const juce::DynamicObject* obj)
{
    if (type == "wrapLibraryList") { pushWrapLibrary(); return; }

    if (type == "wrapSaveToLibrary")
    {
        auto* w = graph.findWrap (obj->getProperty ("wrapId").toString());
        if (w == nullptr) return;
        const auto name = w->name.isNotEmpty() ? w->name : juce::String ("WRAP");
        auto fragment   = buildFragment (w->nodeIds, w->id);
        if (fragment.isVoid()) return;
        auto file = wrapLibraryDir().getChildFile (juce::File::createLegalFileName (name) + ".patchy");
        auto write = [this, file, fragment]
        {
            file.replaceWithText (juce::JSON::toString (fragment, true));
            pushWrapLibrary();
        };
        if (! file.existsAsFile()) { write(); return; }
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle ("Save to library")
            .withMessage ("\"" + file.getFileNameWithoutExtension() + "\" is already in the library. Replace it?")
            .withButton ("Replace").withButton ("Cancel"),
            [write] (int result) { if (result == 1) write(); });
        return;
    }

    // ── v0.0.939: rename / delete library items, choose the folder ──
    if (type == "wrapLibraryRename")
    {
        const juce::File file (obj->getProperty ("file").toString());
        const auto name = obj->getProperty ("name").toString().trim();
        if (name.isEmpty() || ! file.isAChildOf (wrapLibraryDir()) || ! file.existsAsFile()) return;
        const auto target = wrapLibraryDir().getChildFile (juce::File::createLegalFileName (name) + ".patchy");
        if (target != file && target.existsAsFile())
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::WarningIcon)
                .withTitle ("Rename")
                .withMessage ("\"" + target.getFileNameWithoutExtension() + "\" is already in the library.")
                .withButton ("OK"), nullptr);
            return;
        }
        // the name stored in the file follows, so a dropped copy is named the same
        juce::var parsed;
        if (juce::JSON::parse (file.loadFileAsString(), parsed).wasOk())
            if (auto* ws = GraphModel::wrapsOf (*parsed.getDynamicObject()).getArray())
                if (! ws->isEmpty())
                    if (auto* w0 = (*ws)[0].getDynamicObject())
                    {
                        w0->setProperty ("name", name);
                        file.replaceWithText (juce::JSON::toString (parsed, true));
                    }
        if (target != file) file.moveFileTo (target);
        pushWrapLibrary();
        return;
    }

    if (type == "wrapLibraryDelete")
    {
        const juce::File file (obj->getProperty ("file").toString());
        if (! file.isAChildOf (wrapLibraryDir()) || ! file.existsAsFile()) return;
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle ("Delete from the library")
            .withMessage ("Move \"" + file.getFileNameWithoutExtension() + "\" to the Trash?\n\nWraps already placed in projects are not affected.")
            .withButton ("Move to Trash").withButton ("Cancel"),
            [this, file] (int result)
            {
                if (result != 1) return;
                if (! file.moveToTrash()) file.deleteFile();
                pushWrapLibrary();
            });
        return;
    }

    if (type == "wrapLibraryChooseFolder")
    {
        auto chooser = std::make_shared<juce::FileChooser> ("Wrap library folder", wrapLibraryDir());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (dir == juce::File{} || ! dir.isDirectory()) return;
                AppSettings::setWrapLibraryDir (dir == defaultWrapLibraryDir() ? juce::String() : dir.getFullPathName());
                pushWrapLibrary();
            });
        return;
    }

    if (type == "wrapLibraryDefaultFolder")
    {
        AppSettings::setWrapLibraryDir ({});
        pushWrapLibrary();
        return;
    }

    if (type == "wrapLibraryLoad")
    {
        const juce::File file (obj->getProperty ("file").toString());
        if (! file.isAChildOf (wrapLibraryDir()) || ! file.existsAsFile()) return;   // library files only
        juce::var parsed;
        if (juce::JSON::parse (file.loadFileAsString(), parsed).failed()) return;
        auto remapped = remapFragmentIds (parsed, {});
        if (auto* fo = remapped.getDynamicObject())
            commitFragment (*fo, (float) (double) obj->getProperty ("x"), (float) (double) obj->getProperty ("y"));
        return;
    }
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

    // v0.0.938 — wraps: members, face ports and pins follow the new ids
    juce::Array<juce::var> newWraps;
    if (auto* srcWraps = GraphModel::wrapsOf (*srcObj).getArray())
        for (auto& w : *srcWraps)
            if (auto* wObj = w.getDynamicObject())
            {
                auto nw = std::make_unique<juce::DynamicObject>();
                for (auto& prop : wObj->getProperties()) nw->setProperty (prop.name, prop.value);
                auto mapId = [&] (const juce::String& id) { auto it = idMap.find (id); return it != idMap.end() ? it->second : id; };
                juce::Array<juce::var> ids, ports, pins;
                if (auto* a = wObj->getProperty ("nodeIds").getArray()) for (auto& v : *a) ids.add (mapId (v.toString()));
                if (auto* a = wObj->getProperty ("ports").getArray())
                    for (auto& v : *a)
                    {
                        juce::String h = v.toString();
                        for (auto& [oldId, newId] : idMap) if (h.startsWith (oldId)) { h = newId + h.substring (oldId.length()); break; }
                        ports.add (h);
                    }
                if (auto* a = wObj->getProperty ("pins").getArray())
                    for (auto& pv : *a)
                        if (auto* q = pv.getDynamicObject())
                        {
                            auto np = std::make_unique<juce::DynamicObject>();
                            for (auto& prop : q->getProperties()) np->setProperty (prop.name, prop.value);
                            np->setProperty ("nodeId", mapId (q->getProperty ("nodeId").toString()));
                            pins.add (juce::var (np.release()));
                        }
                nw->setProperty ("nodeIds", ids);
                nw->setProperty ("ports",   ports);
                nw->setProperty ("pins",    pins);
                newWraps.add (juce::var (nw.release()));
            }

    auto result = std::make_unique<juce::DynamicObject>();
    result->setProperty ("nodes",       juce::var (newNodes));
    result->setProperty ("connections", juce::var (newConns));
    result->setProperty ("wraps",       juce::var (newWraps));
    result->setProperty ("width",       srcObj->getProperty ("width"));
    result->setProperty ("height",      srcObj->getProperty ("height"));

    return juce::var (result.release());
}
