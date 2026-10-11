// Minimal headless DAW: loads the real SonoRocket.vst3, configures streaming
// through the plugin's saved state (the same path a DAW session uses),
// processes a nasty test signal in varying block sizes, and writes the
// reference input to disk for a bit-exact comparison at the listener.
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <thread>
#include <functional>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String pluginPath = argv[1], room = argv[2], key = argv[3];
    const double seconds = atof (argv[4]);
    const char* refPath = argv[5];
    const double sr = 48000.0;

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> descs;
    fmt.findAllTypesForFile (descs, pluginPath);
    if (descs.isEmpty()) { printf ("no plugin found\n"); return 2; }
    juce::String err;
    auto inst = fmt.createInstanceFromDescription (*descs[0], sr, 512, err);
    if (! inst) { printf ("load failed: %s\n", err.toRawUTF8()); return 2; }
    printf ("loaded %s\n", inst->getName().toRawUTF8());

    inst->setPlayConfigDetails (2, 2, sr, 512);
    inst->prepareToPlay (sr, 2048);

    // configure via state, exactly like a saved session would.
    // Host-side VST3 state is XML <VST3PluginState><IComponent>base64</IComponent>...;
    // the component blob is the plugin's ValueTree followed by JUCE's private trailer.
    auto editState = [&] (std::function<void (juce::ValueTree&)> fn) {
        juce::MemoryBlock mb; inst->getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        if (! xml) { printf ("host state is not XML\n"); exit (2); }
        auto* comp = xml->getChildByName ("IComponent");
        juce::MemoryBlock blob; blob.fromBase64Encoding (comp->getAllSubText());
        juce::MemoryInputStream in (blob, false);
        auto tree = juce::ValueTree::readFromStream (in);
        const auto treeLen = (size_t) in.getPosition();
        fn (tree);
        juce::MemoryOutputStream out; tree.writeToStream (out);
        out.write (static_cast<const char*> (blob.getData()) + treeLen, blob.getSize() - treeLen);
        comp->deleteAllTextElements();
        comp->addTextElement (out.getMemoryBlock().toBase64Encoding());
        juce::MemoryBlock nb; juce::AudioProcessor::copyXmlToBinary (*xml, nb);
        inst->setStateInformation (nb.getData(), (int) nb.getSize());
        return tree;
    };
    if (room != "default") editState ([&] (juce::ValueTree& tree) {
        auto extra = tree.getOrCreateChildWithName ("ExtraState", nullptr);
        extra.setProperty ("RocketHost", "127.0.0.1", nullptr);
        extra.setProperty ("RocketPort", 9100, nullptr);
        extra.setProperty ("RocketRoom", room, nullptr);
        extra.setProperty ("RocketKey", key, nullptr);
        extra.setProperty ("RocketEnabled", true, nullptr);
    });

    {
        juce::ValueTree t2 = editState ([] (juce::ValueTree&) {});
        auto e2 = t2.getChildWithName ("ExtraState");
        printf ("state after set: room=%s enabled=%d  topchildren=%d\n", e2.getProperty ("RocketRoom").toString().toRawUTF8(), (int) e2.getProperty ("RocketEnabled"), t2.getNumChildren());
    }
    // let it connect (pump the message loop a bit)
    for (int i = 0; i < 100; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

    FILE* ref = fopen (refPath, "wb");
    juce::AudioBuffer<float> buf (2, 4096);
    juce::MidiBuffer midi;
    const int sizes[] = { 64, 128, 256, 512, 1024, 2048, 480, 33 };
    uint32_t x = 0x5eed;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x / 4294967296.0; };
    int64_t produced = 0, target = (int64_t) (seconds * sr); int bi = 0;
    std::vector<float> inter (8192);
    auto t0 = std::chrono::steady_clock::now();
    double peakOutDiff = 0, lateDiff = 0; int64_t firstDiffAt = -1, diffCount = 0;
    while (produced < target) {
        int n = (int) std::min<int64_t> (sizes[bi++ % 8], target - produced);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i) {
            const int64_t k = (produced + i) % 1000;
            for (int c = 0; c < 2; ++c) {
                float v;
                if (k < 5) v = 0.0f;
                else if (k < 10) v = (float) ((rnd() - 0.5) * 1e-40);
                else if (k < 20) v = (float) ((rnd() - 0.5) * 1e-9);
                else if (k < 30) v = (float) ((rnd() < 0.5 ? -1 : 1) * (1 + rnd() * 0.5));
                else v = (float) (std::sin ((produced + i) * 0.031 * (c + 1)) * 0.5 + (rnd() - 0.5) * 0.4);
                buf.setSample (c, i, v);
                inter[(size_t) (2 * i + c)] = v;
            }
        }
        fwrite (inter.data(), sizeof (float), (size_t) (2 * n), ref);
        inst->processBlock (buf, midi);
        // plugin must pass the master through untouched (dry = 1, receive muted)
        for (int i = 0; i < n; ++i) for (int c = 0; c < 2; ++c)
        {
            const double d = std::abs (buf.getSample (c, i) - inter[(size_t) (2 * i + c)]);
            peakOutDiff = std::max (peakOutDiff, d);
            if (d != 0) { ++diffCount; if (firstDiffAt < 0) firstDiffAt = produced + i; if (produced > sr) lateDiff = std::max (lateDiff, d); }
        }
        produced += n;
        std::this_thread::sleep_until (t0 + std::chrono::microseconds ((int64_t) (produced / sr * 1e6)));
    }
    fclose (ref);
    for (int i = 0; i < 50; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    printf ("{\"produced\":%lld,\"passThroughMaxDiff\":%g,\"diffAfter1s\":%g,\"firstDiffAt\":%lld,\"diffSamples\":%lld}\n", (long long) produced, peakOutDiff, lateDiff, (long long) firstDiffAt, (long long) diffCount);
    inst->releaseResources();
    inst.reset();
    return 0;
}
