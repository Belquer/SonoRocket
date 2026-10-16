// SonoRocket (capture app): streams any audio input, bit-exact, to any browser.
// For setups where a plugin can't go on the master (old Pro Tools / RTAS,
// a second "streaming" computer fed by cable, any app at all).
//
// Double-click → it starts its own relay + public tunnel (bundled next to the
// executable), opens the last-used audio input, and shows the link to share.

#include <juce_audio_utils/juce_audio_utils.h>
#include "../RocketStreamer.h"
#include "Talkback.h"

#if JUCE_WINDOWS
 // declared directly: windows.h would clash with juce::Rectangle
 extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
 static int currentPid() { return (int) GetCurrentProcessId(); }
#else
 #include <unistd.h>
 static int currentPid() { return (int) getpid(); }
#endif

#ifndef SONOROCKET_VERSION
 #define SONOROCKET_VERSION "dev"
#endif

using namespace juce;
using State = SonoRocket::RocketStreamer::State;

static const Colour kBg (0xff0b0b0c), kPanel (0xff151517), kLine (0xff26262a), kText (0xfff2f0eb),
                    kDim (0xff8d8a84), kOrange (0xffff5a1f), kGreen (0xff3ddc84), kAmber (0xffffc23d), kRed (0xffff4d4d);

static String u8 (const char* s) { return String::fromUTF8 (s); }

// app.log next to the settings: what happened, for remote support ("me manda o app.log")
static void trace (const String& msg)
{
    if (auto* l = Logger::getCurrentLogger()) l->writeToLog (Time::getCurrentTime().toString (false, true, true, true) + "  " + msg);
}

//==============================================================================
// Owns the relay process: starts it (unless one is already running), polls /status,
// and asks it to quit on exit. The relay also watches our PID, so a crash never
// leaves it (or its tunnel) running.
class RelayManager : private Thread
{
public:
    struct Status { bool reachable = false; String link, tunnel; bool senderOnline = false; int listeners = 0; String error; };

    RelayManager() : Thread ("relay manager") {}
    ~RelayManager() override { stop(); }

    void start (const String& room, const File& logFile)
    {
        roomName = room; log = logFile;
        startThread();
    }

    void stop()
    {
        signalThreadShouldExit();
        notify();
        stopThread (3000);
        if (weStartedIt) {
            URL ("http://127.0.0.1:8080/quit").readEntireTextStream();   // relay stops the tunnel too
            if (process && process->isRunning()) { Thread::sleep (500); if (process->isRunning()) process->kill(); }
            weStartedIt = false;
        }
    }

    Status getStatus() const { const ScopedLock sl (lock); return status; }

private:
    static File findHelper (const String& name)
    {
        const auto exeDir = File::getSpecialLocation (File::currentExecutableFile).getParentDirectory();
       #if JUCE_WINDOWS
        const String fileName = name + ".exe";
       #else
        const String fileName = name;
       #endif
        for (auto dir : { exeDir, exeDir.getSiblingFile ("Resources"), exeDir.getChildFile ("bin") })
            if (dir.getChildFile (fileName).existsAsFile())
                return dir.getChildFile (fileName);
        return {};
    }

    static String get (const String& path, int timeoutMs = 1500)
    {
        auto opts = URL::InputStreamOptions (URL::ParameterHandling::inAddress).withConnectionTimeoutMs (timeoutMs);
        if (auto in = URL ("http://127.0.0.1:8080" + path).createInputStream (opts))
            return in->readEntireStreamAsString();
        return {};
    }

    void launch()
    {
        const auto relay = findHelper ("rocket-relay");
        if (! relay.existsAsFile()) { setError (u8 ("rocket-relay não encontrado ao lado do app")); return; }
       #if ! JUCE_WINDOWS
        relay.setExecutePermission (true);
        findHelper ("cloudflared").setExecutePermission (true);
       #endif
        StringArray args { relay.getFullPathName(), "-http", "127.0.0.1:8080", "-room", roomName,
                           "-parent-pid", String (currentPid()), "-log", log.getFullPathName() };
        process = std::make_unique<ChildProcess>();
        trace ("relay: launching " + relay.getFullPathName());
        if (process->start (args, 0)) weStartedIt = true;
        else { trace ("relay: launch FAILED"); setError (u8 ("não consegui iniciar o relay")); }
    }

    void setError (const String& e) { const ScopedLock sl (lock); status.error = e; }

    void run() override
    {
        int launchAttempts = 0;
        while (! threadShouldExit())
        {
            const auto health = get ("/health", 500);
            if (health != "ok") {
                if (! weStartedIt || (process && ! process->isRunning())) {
                    if (launchAttempts++ < 5) { weStartedIt = false; launch(); }
                }
                { const ScopedLock sl (lock); status.reachable = false; }
                wait (700);
                continue;
            }
            const auto js = JSON::parse (get ("/status"));
            {
                const ScopedLock sl (lock);
                status.reachable = true;
                status.error.clear();
                status.link = js.getProperty ("link", {}).toString();
                status.tunnel = js.getProperty ("tunnel", {}).toString();
                status.senderOnline = (bool) js.getProperty ("senderOnline", false);
                status.listeners = (int) js.getProperty ("listeners", 0);
            }
            wait (1000);
        }
    }

    String roomName;
    File log;
    std::unique_ptr<ChildProcess> process;
    bool weStartedIt = false;
    CriticalSection lock;
    Status status;
};

//==============================================================================
class LevelMeter : public Component
{
public:
    void setLevel (float peak) { level = peak; repaint(); }
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (kBg); g.fillRoundedRectangle (r, 4.0f);
        const float db = level > 0 ? 20.0f * std::log10 (level) : -100.0f;
        const float frac = jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        ColourGradient grad (kGreen, r.getX(), 0, kRed, r.getRight(), 0, false);
        grad.addColour (0.8, kAmber);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r.withWidth (r.getWidth() * frac), 4.0f);
        if (level > 1.0f) { g.setColour (kRed); g.fillRect (r.removeFromRight (6)); }
    }
private:
    float level = 0;
};

//==============================================================================
// Hold-to-talk button: talks while the mouse (or finger) is down.
class HoldButton : public TextButton
{
public:
    using TextButton::TextButton;
    bool held() const { return isDown(); }
};

//==============================================================================
class MainComponent : public Component, private Timer, private AudioIODeviceCallback, private ChangeListener,
                      private MidiInputCallback
{
public:
    MainComponent()
    {
        PropertiesFile::Options o;
        o.applicationName = "SonoRocket"; o.filenameSuffix = "settings";
       #if JUCE_LINUX
        o.folderName = ".config/SonoRocket";
       #else
        o.folderName = "SonoRocket";
       #endif
        o.osxLibrarySubFolder = "Application Support";
        props = std::make_unique<PropertiesFile> (o);
        fileLogger.reset (new FileLogger (props->getFile().getSiblingFile ("app.log"), "SonoRocket " SONOROCKET_VERSION));
        Logger::setCurrentLogger (fileLogger.get());
        trace ("settings: " + props->getFile().getFullPathName());

        // stable room per computer: the listener link stays predictable
        if (props->getValue ("room").isEmpty())
            props->setValue ("room", "mix-" + String::toHexString (Random().nextInt64()).substring (0, 8));

        // --- audio input
        std::unique_ptr<XmlElement> saved (props->getXmlValue ("audio"));
        // every input and output is opened; which ones are the master, the talkback mic
        // and the talkback output is chosen below (and remembered)
        const auto audioErr = deviceManager.initialise (kMaxCh, kMaxCh, saved.get(), true);
        if (! props->getBoolValue ("talkOutChosen", false)) {
            // never grab a Windows output (exclusive mode) until the talkback asks for one
            auto setup = deviceManager.getAudioDeviceSetup();
            if (setup.outputDeviceName.isNotEmpty()) { setup.outputDeviceName = {}; deviceManager.setAudioDeviceSetup (setup, true); }
        }
        openAllChannels();
       #if JUCE_WINDOWS
        if (saved == nullptr) {
            // first run: exclusive mode bypasses the Windows mixer (no resampling, no effects)
            for (auto* t : deviceManager.getAvailableDeviceTypes())
                if (t->getTypeName() == "Windows Audio (Exclusive Mode)") { deviceManager.setCurrentAudioDeviceType (t->getTypeName(), true); break; }
        }
       #endif
        trace ("audio: type=" + deviceManager.getCurrentAudioDeviceType() + " device=" + (deviceManager.getCurrentAudioDevice() ? deviceManager.getCurrentAudioDevice()->getName() : String ("none"))
               + (audioErr.isNotEmpty() ? " error=" + audioErr : String()));
        deviceManager.addAudioCallback (this);
        deviceManager.addChangeListener (this);

        // min = max = kMaxCh hides the channel tick-boxes: channels are picked in the Talkback panel
        selector = std::make_unique<AudioDeviceSelectorComponent> (deviceManager, kMaxCh, kMaxCh, kMaxCh, kMaxCh, false, false, true, false);
        addAndMakeVisible (*selector);

        // --- streamer (to our own relay on this computer)
        SonoRocket::RocketStreamer::Config c;
        c.enabled = props->getBoolValue ("enabled", true);
        streamer.setConfig (c);

        auto logFile = props->getFile().getSiblingFile ("relay.log");
        relay.start (props->getValue ("room"), logFile);

        // --- UI
        title.setText ("SonoRocket", dontSendNotification);
        title.setFont (Font (26.0f, Font::bold));
        title.setColour (Label::textColourId, kOrange);
        addAndMakeVisible (title);

        version.setText ("v" SONOROCKET_VERSION, dontSendNotification);
        version.setColour (Label::textColourId, kDim);
        version.setJustificationType (Justification::centredRight);
        addAndMakeVisible (version);

        status.setFont (Font (17.0f, Font::bold));
        addAndMakeVisible (status);
        detail.setColour (Label::textColourId, kDim);
        detail.setFont (Font (Font::getDefaultMonospacedFontName(), 13.0f, Font::plain));
        addAndMakeVisible (detail);

        inputTitle.setText (u8 ("Placa de áudio (por onde chega o som do Pro Tools)"), dontSendNotification);
        inputTitle.setColour (Label::textColourId, kDim);
        addAndMakeVisible (inputTitle);
        addAndMakeVisible (meterL); addAndMakeVisible (meterR);
        for (auto* l : { &labL, &labR, &dbL, &dbR }) { l->setColour (Label::textColourId, kDim); l->setFont (Font (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain)); addAndMakeVisible (*l); }
        labL.setText ("L", dontSendNotification); labR.setText ("R", dontSendNotification);

        link.setColour (Label::backgroundColourId, kBg);
        link.setColour (Label::textColourId, kText);
        link.setFont (Font (Font::getDefaultMonospacedFontName(), 13.0f, Font::plain));
        link.setBorderSize (BorderSize<int> (8, 10, 8, 10));
        addAndMakeVisible (link);

        styleButton (copyBtn, kLine, kText);
        SafePointer<MainComponent> safeThis (this);
        copyBtn.onClick = [this, safeThis] {
            if (currentLink.isEmpty()) return;
            SystemClipboard::copyTextToClipboard (currentLink);
            copyBtn.setButtonText ("Copiado!");
            Timer::callAfterDelay (1500, [safeThis] { if (safeThis != nullptr) safeThis->copyBtn.setButtonText ("Copiar link"); });
        };
        styleButton (whatsBtn, Colour (0xff25d366), Colours::black);
        whatsBtn.onClick = [this] {
            if (currentLink.isNotEmpty())
                URL ("https://wa.me/?text=" + URL::addEscapeChars (u8 ("Mix ao vivo, qualidade máxima: ") + currentLink, true)).launchInDefaultBrowser();
        };
        styleButton (streamBtn, kOrange, Colours::black);
        streamBtn.onClick = [this] {
            auto c2 = streamer.getConfig();
            c2.enabled = ! c2.enabled;
            streamer.setConfig (c2);
            props->setValue ("enabled", c2.enabled);
        };
        addAndMakeVisible (copyBtn); addAndMakeVisible (whatsBtn); addAndMakeVisible (streamBtn);

        tip.setText (u8 ("Dica: use \"Windows Audio (Exclusive Mode)\" ou ASIO e a mesma taxa (ex.: 48000) do Pro Tools. Pelo S/PDIF chega bit a bit."), dontSendNotification);
        tip.setColour (Label::textColourId, kDim);
        tip.setFont (Font (12.0f));
        tip.setMinimumHorizontalScale (1.0f);   // wrap instead of squeezing
        addAndMakeVisible (tip);

        // --- talkback
        masterStart = props->getIntValue ("masterStart", 0);
        micChannel  = props->getIntValue ("talkMic", -1);
        outStart    = props->getIntValue ("talkOut", -1);
        talkback.setPlaybackGain (Decibels::decibelsToGain ((float) props->getDoubleValue ("talkVol", 0.0)));

        talkTitle.setText (u8 ("Talkback (fala com o dB, sem os 2 s de atraso)"), dontSendNotification);
        talkTitle.setColour (Label::textColourId, kDim);
        addAndMakeVisible (talkTitle);
        for (auto* l : { &masterLab, &micLab, &outLab, &volLab }) {
            l->setColour (Label::textColourId, kDim); l->setFont (Font (13.0f)); addAndMakeVisible (*l);
        }
        masterLab.setText ("Master (Pro Tools)", dontSendNotification);
        micLab.setText ("Microfone", dontSendNotification);
        outLab.setText (u8 ("Voz do dB sai em"), dontSendNotification);
        volLab.setText ("Volume da voz", dontSendNotification);
        for (auto* b : { &masterBox, &micBox, &outBox }) addAndMakeVisible (*b);
        masterBox.onChange = [this] { masterStart = jmax (0, (masterBox.getSelectedId() - 1) * 2); props->setValue ("masterStart", masterStart.load()); updateStreamFormat(); };
        micBox.onChange = [this] { micChannel = micBox.getSelectedId() - 2; props->setValue ("talkMic", micChannel.load()); updateTalkEnabled(); };
        outBox.onChange = [this] { outStart = outBox.getSelectedId() >= 2 ? (outBox.getSelectedId() - 2) * 2 : -1; props->setValue ("talkOut", outStart.load()); updateTalkEnabled(); };
        talkVol.setSliderStyle (Slider::LinearHorizontal);
        talkVol.setTextBoxStyle (Slider::TextBoxRight, false, 60, 20);
        talkVol.setRange (-40.0, 12.0, 0.5);
        talkVol.setTextValueSuffix (" dB");
        talkVol.setValue (props->getDoubleValue ("talkVol", 0.0), dontSendNotification);
        talkVol.setColour (Slider::thumbColourId, kOrange);
        talkVol.setColour (Slider::trackColourId, kOrange.withAlpha (0.5f));
        talkVol.onValueChange = [this] { talkback.setPlaybackGain (Decibels::decibelsToGain ((float) talkVol.getValue())); props->setValue ("talkVol", talkVol.getValue()); };
        addAndMakeVisible (talkVol);
        addAndMakeVisible (micMeter);
        talkState.setFont (Font (15.0f, Font::bold));
        addAndMakeVisible (talkState);
        styleButton (talkBtn, kLine, kText);
        talkBtn.setTriggeredOnMouseDown (false);
        addAndMakeVisible (talkBtn);
        talkHint.setText (u8 ("Com a DAW parada o microfone fica aberto. Tocando: segure T (janela na frente), Ctrl+T (de qualquer lugar do PC), o botão acima ou um pedal MIDI."), dontSendNotification);
        talkHint.setColour (Label::textColourId, kDim);
        talkHint.setFont (Font (12.0f));
        talkHint.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (talkHint);
        refreshChannelBoxes();
        updateTalkEnabled();
        openMidiInputs();

        setSize (560, kContentHeight);
        startTimerHz (30);
        trace ("ui: ready");
    }

    ~MainComponent() override
    {
        trace ("quit");
        stopTimer();
        deviceManager.removeAudioCallback (this);
        deviceManager.removeChangeListener (this);
        midiInputs.clear();
        saveAudioState();
        relay.stop();
        Logger::setCurrentLogger (nullptr);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (kBg);
        g.setColour (kPanel);
        g.fillRoundedRectangle (inputPanel.toFloat(), 12.0f);
        g.fillRoundedRectangle (sharePanel.toFloat(), 12.0f);
        g.fillRoundedRectangle (talkPanel.toFloat(), 12.0f);
    }

    static constexpr int kContentHeight = 930;
    static constexpr int kMaxCh = 64;

    void resized() override
    {
        auto b = getLocalBounds().reduced (16);
        auto top = b.removeFromTop (34);
        version.setBounds (top.removeFromRight (90));
        title.setBounds (top);
        status.setBounds (b.removeFromTop (26));
        detail.setBounds (b.removeFromTop (20));
        b.removeFromTop (10);

        sharePanel = b.removeFromTop (170);
        {
            auto s = sharePanel.reduced (12);
            link.setBounds (s.removeFromTop (36));
            s.removeFromTop (10);
            auto row = s.removeFromTop (40);
            copyBtn.setBounds (row.removeFromLeft (row.getWidth() / 2 - 5));
            row.removeFromLeft (10);
            whatsBtn.setBounds (row);
            s.removeFromTop (10);
            streamBtn.setBounds (s.removeFromTop (44));
        }
        b.removeFromTop (12);

        talkPanel = b.removeFromTop (250);
        {
            auto t = talkPanel.reduced (12);
            talkTitle.setBounds (t.removeFromTop (22));
            auto row = [&] (Label& l, Component& c) { auto r = t.removeFromTop (26); l.setBounds (r.removeFromLeft (130)); c.setBounds (r); t.removeFromTop (4); };
            row (masterLab, masterBox);
            row (micLab, micBox);
            { auto r = t.removeFromTop (8); micMeter.setBounds (r.withTrimmedLeft (130).reduced (0, 1)); t.removeFromTop (4); }
            row (outLab, outBox);
            row (volLab, talkVol);
            auto last = t.removeFromTop (36);
            talkBtn.setBounds (last.removeFromRight (200));
            last.removeFromRight (8);
            talkState.setBounds (last);
            talkHint.setBounds (t);
        }
        b.removeFromTop (12);

        inputPanel = b;
        auto p = inputPanel.reduced (12);
        inputTitle.setBounds (p.removeFromTop (22));
        auto meterRow = [&] (Label& lab, LevelMeter& m, Label& db) {
            auto r = p.removeFromTop (16);
            lab.setBounds (r.removeFromLeft (16));
            db.setBounds (r.removeFromRight (56));
            m.setBounds (r.reduced (4, 3));
            p.removeFromTop (4);
        };
        meterRow (labL, meterL, dbL);
        meterRow (labR, meterR, dbR);
        tip.setBounds (p.removeFromBottom (34));
        selector->setBounds (p);
    }

private:
    static void styleButton (TextButton& b, Colour bg, Colour fg)
    {
        b.setColour (TextButton::buttonColourId, bg);
        b.setColour (TextButton::textColourOffId, fg);
    }

    // ---- audio
    void audioDeviceAboutToStart (AudioIODevice* d) override
    {
        devIns = d->getActiveInputChannels().countNumberOfSetBits();
        devOuts = d->getActiveOutputChannels().countNumberOfSetBits();
        const int chans = masterStart.load() + 1 < devIns ? 2 : 1;
        trace ("audio: started " + d->getName() + " @ " + String (d->getCurrentSampleRate()) + " Hz, " + String (devIns.load()) + " in / " + String (devOuts.load()) + " out");
        streamer.prepare (d->getCurrentSampleRate(), chans);
        talkback.prepare (d->getCurrentSampleRate());
        sampleRate = d->getCurrentSampleRate();
    }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int n, const AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c) if (out[c]) FloatVectorOperations::clear (out[c], n);
        if (numIn <= 0 || n <= 0) return;

        // master: the chosen input pair, streamed untouched
        int m0 = masterStart.load();
        if (m0 >= numIn) m0 = 0;
        const int chans = m0 + 1 < numIn ? 2 : 1;
        const float* master[2] = { in[m0], in[m0 + chans - 1] };
        streamer.pushAudio (master, chans, n);   // raw samples, untouched

        float loud = 0.0f;
        for (int c = 0; c < chans; ++c) {
            auto r = FloatVectorOperations::findMinAndMax (master[c], n);
            const float pk = jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
            loud = jmax (loud, pk);
            float prev = peak[c].load();
            while (pk > prev && ! peak[c].compare_exchange_weak (prev, pk)) {}
        }
        if (chans == 1) peak[1].store (peak[0].load());
        if (loud > kPlayingThreshold) dawLoud = true;

        // talkback: mic on its own input, the master as echo reference, voice to an output pair
        const int mic = micChannel.load(), o0 = outStart.load();
        float* voiceOut = (o0 >= 0 && o0 < numOut) ? out[o0] : nullptr;
        talkback.process ((mic >= 0 && mic < numIn) ? in[mic] : nullptr, master[0], master[1], voiceOut, n);
        if (voiceOut != nullptr && o0 + 1 < numOut && out[o0 + 1] != nullptr)
            FloatVectorOperations::copy (out[o0 + 1], voiceOut, n);
    }

    void changeListenerCallback (ChangeBroadcaster*) override
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        if (setup.outputDeviceName.isNotEmpty()) props->setValue ("talkOutChosen", true);
        openAllChannels();
        refreshChannelBoxes();
        saveAudioState();
    }

    // every channel open, so in[i] / out[i] in the callback are hardware channel i
    void openAllChannels()
    {
        auto* dev = deviceManager.getCurrentAudioDevice();
        if (dev == nullptr) return;
        auto setup = deviceManager.getAudioDeviceSetup();
        BigInteger ins, outs;
        ins.setRange (0, jmin (kMaxCh, dev->getInputChannelNames().size()), true);
        outs.setRange (0, jmin (kMaxCh, dev->getOutputChannelNames().size()), true);
        if (setup.inputChannels == ins && setup.outputChannels == outs && ! setup.useDefaultInputChannels && ! setup.useDefaultOutputChannels)
            return;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        setup.inputChannels = ins; setup.outputChannels = outs;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    void refreshChannelBoxes()
    {
        auto* dev = deviceManager.getCurrentAudioDevice();
        const auto ins = dev ? dev->getInputChannelNames() : StringArray();
        const auto outs = dev ? dev->getOutputChannelNames() : StringArray();
        auto nameOf = [] (const StringArray& a, int i) { return String (i + 1) + (a[i].isNotEmpty() && a[i] != String (i + 1) ? " (" + a[i] + ")" : String()); };

        masterBox.clear (dontSendNotification);
        for (int i = 0; i < jmax (1, ins.size()); i += 2)
            masterBox.addItem (i + 1 < ins.size() ? "Entradas " + String (i + 1) + "-" + String (i + 2) : "Entrada " + String (i + 1), i / 2 + 1);
        masterBox.setSelectedId (jlimit (1, jmax (1, masterBox.getNumItems()), masterStart.load() / 2 + 1), dontSendNotification);

        micBox.clear (dontSendNotification);
        micBox.addItem ("Desligado", 1);
        for (int i = 0; i < ins.size(); ++i) micBox.addItem ("Entrada " + nameOf (ins, i), i + 2);
        micBox.setSelectedId (micChannel.load() >= 0 && micChannel.load() < ins.size() ? micChannel.load() + 2 : 1, dontSendNotification);

        outBox.clear (dontSendNotification);
        outBox.addItem (outs.isEmpty() ? u8 ("Desligado (escolha a saída lá embaixo)") : String ("Desligado"), 1);
        for (int i = 0; i < outs.size(); i += 2)
            outBox.addItem (i + 1 < outs.size() ? u8 ("Saídas ") + String (i + 1) + "-" + String (i + 2) : u8 ("Saída ") + String (i + 1), i / 2 + 2);
        outBox.setSelectedId (outStart.load() >= 0 && outStart.load() < outs.size() ? outStart.load() / 2 + 2 : 1, dontSendNotification);
        updateTalkEnabled();
    }

    void updateStreamFormat()
    {
        if (auto* d = deviceManager.getCurrentAudioDevice())
            streamer.prepare (d->getCurrentSampleRate(), masterStart.load() + 1 < devIns ? 2 : 1);
    }

    void updateTalkEnabled()
    {
        const bool on = micChannel.load() >= 0 || outStart.load() >= 0;
        if (on != streamer.isTalkbackEnabled()) trace (String ("talkback: ") + (on ? "on" : "off"));
        streamer.setTalkbackEnabled (on);
    }

    // ---- MIDI pedal / button: any note held, or sustain (CC64) down, = hold to talk
    void openMidiInputs()
    {
        for (auto& info : MidiInput::getAvailableDevices()) {
            bool have = false;
            for (auto& m : midiInputs) if (m->getIdentifier() == info.identifier) have = true;
            if (have) continue;
            if (auto m = MidiInput::openDevice (info.identifier, this)) {
                trace ("midi: " + info.name);
                m->start();
                midiInputs.push_back (std::move (m));
            }
        }
    }
    void handleIncomingMidiMessage (MidiInput*, const MidiMessage& m) override
    {
        if (m.isNoteOn()) ++midiNotes;
        else if (m.isNoteOff()) midiNotes = jmax (0, midiNotes.load() - 1);
        else if (m.isSustainPedalOn()) midiPedal = true;
        else if (m.isSustainPedalOff()) midiPedal = false;
        else if (m.isAllNotesOff() || m.isAllSoundOff()) midiNotes = 0;
    }

    void updateTalk()
    {
        const auto now = Time::getMillisecondCounter();
        if (dawLoud.exchange (false)) lastLoudMs = now;
        const bool playing = now - lastLoudMs < kPlayingHoldMs;
        // T with this window in front, or Ctrl+T from anywhere on this computer
        // (polled, so it works whatever app has the focus)
        const bool tDown = KeyPress::isKeyCurrentlyDown ('t');
       #if JUCE_WINDOWS
        const bool ctrl = KeyPress::isKeyCurrentlyDown (0x11);   // VK_CONTROL via GetAsyncKeyState: global, unlike the modifier cache
       #else
        const bool ctrl = ModifierKeys::getCurrentModifiersRealtime().isCtrlDown();
       #endif
        const bool keyT = tDown && (Process::isForegroundProcess() || ctrl);
        const bool ptt = keyT || talkBtn.held() || midiNotes.load() > 0 || midiPedal.load();
        const bool haveMic = micChannel.load() >= 0 && micChannel.load() < devIns;
        talkback.setMicOpen (haveMic && streamer.getState() == State::Streaming && (! playing || ptt));

        micHeld = jmax (talkback.takeMicPeak(), micHeld * 0.85f);
        micMeter.setLevel (micHeld);
        if ((ticks % 3) != 0) return;
        String s; Colour c = kDim;
        if (! haveMic)                    s = u8 ("Escolha a entrada do microfone");
        else if (talkback.isSending())    { s = playing ? u8 ("● falando (segurando)") : u8 ("● mic aberto · DAW parada"); c = kGreen; }
        else if (playing)                 { s = u8 ("DAW tocando · segure T ou Ctrl+T para falar"); c = kAmber; }
        else                              s = u8 ("mic fechado");
        if (talkback.takeVoicePeak() > 0.003f) { s << u8 ("  ·  dB falando"); c = kOrange; }
        talkState.setText (s, dontSendNotification);
        talkState.setColour (Label::textColourId, c);
        talkBtn.setButtonText (talkback.isSending() && ptt ? "Falando..." : "Segure para falar (Ctrl+T)");
    }
    void saveAudioState()
    {
        if (auto xml = deviceManager.createStateXml()) props->setValue ("audio", xml.get());
        props->saveIfNeeded();
    }

    // ---- UI refresh
    void timerCallback() override
    {
        ++ticks;
        updateTalk();                         // 30 Hz: hold-to-talk must feel instant
        if (ticks % 90 == 0) openMidiInputs();  // pedal plugged in later
        if ((ticks % 3) != 0) return;         // the rest at 10 Hz
        if (ticks <= 9 || ticks % 1800 == 0) trace ("tick " + String (ticks));
        for (int c = 0; c < 2; ++c) {
            const float pk = peak[c].exchange (0.0f);
            heldPeak[c] = jmax (pk, heldPeak[c] * 0.85f);
        }
        meterL.setLevel (heldPeak[0]); meterR.setLevel (heldPeak[1]);
        auto dbs = [] (float v) { if (v <= 0) return String (u8 ("−∞")); const float d = 20 * std::log10 (v); return (d > 0 ? "+" : "") + String (d, 1); };
        dbL.setText (dbs (heldPeak[0]), dontSendNotification);
        dbR.setText (dbs (heldPeak[1]), dontSendNotification);

        const auto rs = relay.getStatus();
        const auto st = streamer.getState();
        const bool enabled = streamer.getConfig().enabled;
        const bool publicLink = rs.link.startsWith ("https://") && (rs.tunnel == "up" || rs.tunnel == "fixed");
        currentLink = publicLink ? rs.link : String();

        streamBtn.setButtonText (enabled ? "Parar" : "Transmitir");
        String s, d;
        Colour col = kAmber;
        if (rs.error.isNotEmpty())                  { s = "Erro: " + rs.error; col = kRed; }
        else if (! rs.reachable)                    { s = "Ligando..."; }
        else if (! enabled)                         { s = "Desligado"; col = kDim; }
        else if (st != State::Streaming)            { s = "Conectando..."; }
        else if (! publicLink)                      { s = u8 ("AO VIVO · gerando link público…"); }
        else if (deviceManager.getCurrentAudioDevice() == nullptr) { s = u8 ("Escolha a entrada de áudio aqui embaixo"); }
        else if (heldPeak[0] < 1e-6f && heldPeak[1] < 1e-6f) { s = u8 ("AO VIVO · sem sinal na entrada"); }
        else                                        { s = "AO VIVO"; col = kGreen; }

        if (st == State::Streaming)
            d << String (sampleRate, 0) << " Hz · float32 · " << rs.listeners << (rs.listeners == 1 ? " ouvindo" : " ouvindo")
              << " · fila " << String (streamer.getFifoSeconds(), 2) << " s";
        if (streamer.getFramesDropped() > 0) d << " · PERDEU " << String (streamer.getFramesDropped());

        if (s != lastStatus) { trace ("status: " + s + (currentLink.isNotEmpty() ? "  " + currentLink : String())); lastStatus = s; }
        status.setText (u8 ("● ") + s, dontSendNotification);
        status.setColour (Label::textColourId, col);
        detail.setText (d, dontSendNotification);
        link.setText (currentLink.isNotEmpty() ? currentLink : u8 ("o link aparece aqui em alguns segundos"), dontSendNotification);
        link.setColour (Label::textColourId, currentLink.isNotEmpty() ? kText : kDim);
        copyBtn.setEnabled (currentLink.isNotEmpty());
        whatsBtn.setEnabled (currentLink.isNotEmpty());
    }

    std::unique_ptr<FileLogger> fileLogger;
    int ticks = 0;
    String lastStatus;
    std::unique_ptr<PropertiesFile> props;
    AudioDeviceManager deviceManager;
    std::unique_ptr<AudioDeviceSelectorComponent> selector;
    SonoRocket::RocketStreamer streamer;
    SonoRocket::Talkback talkback { streamer };
    RelayManager relay;
    std::vector<std::unique_ptr<MidiInput>> midiInputs;
    std::atomic<int> masterStart { 0 }, micChannel { -1 }, outStart { -1 };
    std::atomic<int> devIns { 0 }, devOuts { 0 };
    std::atomic<bool> dawLoud { false }, midiPedal { false };
    std::atomic<int> midiNotes { 0 };
    juce::uint32 lastLoudMs = 0;
    float micHeld = 0.0f;
    static constexpr float kPlayingThreshold = 0.001f;   // −60 dBFS: above this the DAW is "rolling"
    static constexpr juce::uint32 kPlayingHoldMs = 700;  // silent this long = stopped, mic opens
    std::atomic<float> peak[2] { {0.0f}, {0.0f} };
    float heldPeak[2] { 0, 0 };
    double sampleRate = 48000;
    String currentLink;

    Label title, version, status, detail, inputTitle, labL, labR, dbL, dbR, link, tip;
    LevelMeter meterL, meterR, micMeter;
    Label talkTitle, masterLab, micLab, outLab, volLab, talkState, talkHint;
    ComboBox masterBox, micBox, outBox;
    Slider talkVol;
    HoldButton talkBtn { "Segure para falar (Ctrl+T)" };
    TextButton copyBtn { "Copiar link" }, whatsBtn { "Mandar no WhatsApp" }, streamBtn { "Parar" };
    juce::Rectangle<int> inputPanel, sharePanel, talkPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class SonoRocketApp : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "SonoRocket"; }
    const String getApplicationVersion() override { return SONOROCKET_VERSION; }
    bool moreThanOneInstanceAllowed() override    { return false; }

    void initialise (const String&) override
    {
        LookAndFeel::getDefaultLookAndFeel().setColour (ResizableWindow::backgroundColourId, kBg);
        window = std::make_unique<MainWindow>();
    }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const String&) override { if (window) window->toFront (true); }

private:
    struct MainWindow : DocumentWindow
    {
        MainWindow() : DocumentWindow ("SonoRocket", kBg, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new Scroller(), true);
            setResizable (true, false);
            setResizeLimits (480, 420, 900, 1400);
            const auto area = Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                                ? Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea : juce::Rectangle<int> (0, 0, 1366, 728);
            setSize (560, jmin (MainComponent::kContentHeight, area.getHeight() - 40));
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    // Scrolls the content on short screens (1366x768 laptops).
    struct Scroller : Component
    {
        Scroller() { vp.setViewedComponent (&content, false); vp.setScrollBarsShown (true, false); addAndMakeVisible (vp); setSize (560, MainComponent::kContentHeight); }
        void resized() override
        {
            vp.setBounds (getLocalBounds());
            content.setSize (vp.getMaximumVisibleWidth(), jmax (getHeight(), MainComponent::kContentHeight));
        }
        Viewport vp;
        MainComponent content;
    };
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SonoRocketApp)
