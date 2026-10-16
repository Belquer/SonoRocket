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
        // Before the talkback, the master pair was picked with channel tick-boxes and
        // saved in the device state. Carry that choice over so nothing changes for him.
        if (! props->containsKey ("masterStart") && saved != nullptr) {
            BigInteger bits; bits.parseString (saved->getStringAttribute ("audioDeviceInChans"), 2);
            const int first = bits.findNextSetBit (0);
            if (first > 0) { props->setValue ("masterStart", first & ~1); trace ("master: carried over from tick-boxes, input " + String (first + 1)); }
        }
        // every input and output is opened; which ones are the master, the talkback mic
        // and the talkback output is chosen below (and remembered)
        // Capture only: an output here can stop the input (WASAPI exclusive refuses
        // in/out pairs with different buffer sizes). The talkback output has its own device.
        const auto audioErr = deviceManager.initialise (kMaxCh, 0, saved.get(), true);
        {
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
        selector = std::make_unique<AudioDeviceSelectorComponent> (deviceManager, kMaxCh, kMaxCh, 0, 0, false, false, true, false);
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
        whatsBtn.onClick = [this, safeThis] {
            if (currentLink.isEmpty()) return;
            const auto text = u8 ("Mix ao vivo, qualidade máxima: ") + currentLink;
            SystemClipboard::copyTextToClipboard (currentLink);   // whatever happens next, the link is ready to paste
            const auto q = URL::addEscapeChars (text, false);
            // WhatsApp Desktop (its whatsapp: link), else WhatsApp Web
            bool ok = Process::openDocument ("whatsapp://send?text=" + q, {});
            if (! ok) ok = URL ("https://web.whatsapp.com/send?text=" + q).launchInDefaultBrowser();
            trace (String ("whatsapp: ") + (ok ? "opened" : "failed"));
            whatsBtn.setButtonText (u8 ("Link copiado"));
            Timer::callAfterDelay (4000, [safeThis] { if (safeThis != nullptr) safeThis->whatsBtn.setButtonText ("WhatsApp"); });
        };
        styleButton (streamBtn, kOrange, Colours::black);
        streamBtn.onClick = [this] {
            auto c2 = streamer.getConfig();
            c2.enabled = ! c2.enabled;
            streamer.setConfig (c2);
            props->setValue ("enabled", c2.enabled);
        };
        // the studio's video page: camera + the listener's picture, served by our own relay
        styleButton (videoBtn, kLine, kText);
        videoBtn.onClick = [this] {
            URL ("http://127.0.0.1:8080/?room=" + URL::addEscapeChars (props->getValue ("room"), true) + "&studio=1").launchInDefaultBrowser();
            trace ("video: opened studio page");
        };
        addAndMakeVisible (copyBtn); addAndMakeVisible (whatsBtn); addAndMakeVisible (streamBtn); addAndMakeVisible (videoBtn);

        tip.setText (u8 ("Dica: use \"Windows Audio (Exclusive Mode)\" ou ASIO e a mesma taxa (ex.: 48000) do Pro Tools. Pelo S/PDIF chega bit a bit."), dontSendNotification);
        tip.setColour (Label::textColourId, kDim);
        tip.setFont (Font (12.0f));
        tip.setMinimumHorizontalScale (1.0f);   // wrap instead of squeezing
        addAndMakeVisible (tip);

        // --- talkback
        masterStart = props->getIntValue ("masterStart", 0);
        micChannel  = props->getIntValue ("talkMic", -1);
        micDevName  = props->getValue ("talkMicDevice");
        micDevCh    = props->getIntValue ("talkMicCh", 0);
        talkOutName = props->getValue ("talkOutDevice");
        initVoiceOutput();
        initMicInput();
        talkback.setPlaybackGain (Decibels::decibelsToGain ((float) props->getDoubleValue ("talkVol", 0.0)));

        talkTitle.setText (u8 ("Talkback (fala com o dB, sem os 2 s de atraso)"), dontSendNotification);
        talkTitle.setColour (Label::textColourId, kDim);
        addAndMakeVisible (talkTitle);
        for (auto* l : { &masterLab, &micLab, &outLab, &volLab }) {
            l->setColour (Label::textColourId, kDim); l->setFont (Font (13.0f)); addAndMakeVisible (*l);
        }
        masterLab.setText ("Master (Pro Tools)", dontSendNotification);
        micLab.setText ("Microfone", dontSendNotification);
        outLab.setText (u8 ("Voz do dB sai em"), dontSendNotification);   // its own output device
        volLab.setText ("Volume da voz", dontSendNotification);
        for (auto* b : { &masterBox, &micBox, &outBox }) addAndMakeVisible (*b);
        masterBox.onChange = [this] { setMaster (jmax (0, (masterBox.getSelectedId() - 1) * 2), true); };
        micBox.onChange = [this] { chooseMic (micBox.getSelectedId()); };
        outBox.onChange = [this] {
            talkOutName = outBox.getSelectedId() >= 2 ? outBox.getText() : String();
            props->setValue ("talkOutDevice", talkOutName);
            openVoiceOutput();
            updateTalkEnabled();
        };
        talkVol.setSliderStyle (Slider::LinearHorizontal);
        talkVol.setTextBoxStyle (Slider::TextBoxRight, false, 60, 20);
        talkVol.setRange (-40.0, 12.0, 0.5);
        talkVol.setTextValueSuffix (" dB");
        talkVol.setValue (props->getDoubleValue ("talkVol", 0.0), dontSendNotification);
        talkVol.setColour (Slider::thumbColourId, kOrange);
        talkVol.setColour (Slider::trackColourId, kOrange.withAlpha (0.5f));
        talkVol.onValueChange = [this] { talkback.setPlaybackGain (Decibels::decibelsToGain ((float) talkVol.getValue())); props->setValue ("talkVol", talkVol.getValue()); };
        addAndMakeVisible (talkVol);
        styleButton (testBtn, kLine, kText);
        testBtn.onClick = [this] { talkback.playTestTone(); trace ("talkback out: test tone"); };
        addAndMakeVisible (testBtn);
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
        micIn.removeAudioCallback (&micInCb);
        micIn.closeAudioDevice();
        voiceOut.removeAudioCallback (&voiceOutCb);
        voiceOut.closeAudioDevice();
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
            const int w3 = (row.getWidth() - 20) / 3;
            copyBtn.setBounds (row.removeFromLeft (w3));
            row.removeFromLeft (10);
            whatsBtn.setBounds (row.removeFromLeft (w3));
            row.removeFromLeft (10);
            videoBtn.setBounds (row);
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
            { auto r = t.removeFromTop (26); outLab.setBounds (r.removeFromLeft (130)); testBtn.setBounds (r.removeFromRight (70)); r.removeFromRight (6); outBox.setBounds (r); t.removeFromTop (4); }
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
        const int chans = masterStart.load() + 1 < devIns ? 2 : 1;
        trace ("audio: started " + d->getName() + " @ " + String (d->getCurrentSampleRate()) + " Hz, " + String (devIns.load()) + " in");
        streamer.prepare (d->getCurrentSampleRate(), chans);
        talkback.prepareReference (d->getCurrentSampleRate());
        if (micChannel.load() >= 0) talkback.prepareMic (d->getCurrentSampleRate());
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

        // every input's level: to find where the Pro Tools signal really arrives
        for (int c = 0; c < jmin (numIn, kMaxCh); ++c) {
            auto r = FloatVectorOperations::findMinAndMax (in[c], n);
            const float pk = jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
            float prev = chanPeak[c].load();
            while (pk > prev && ! chanPeak[c].compare_exchange_weak (prev, pk)) {}
        }

        // talkback: the master is the echo reference; the mic may be another input here
        talkback.captureReference (master[0], master[1], n);
        const int mic = micChannel.load();
        if (mic >= 0 && mic < numIn) talkback.captureMic (in[mic], n);
    }

    void changeListenerCallback (ChangeBroadcaster*) override
    {
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
        BigInteger ins;
        ins.setRange (0, jmin (kMaxCh, dev->getInputChannelNames().size()), true);
        if (setup.inputChannels == ins && ! setup.useDefaultInputChannels)
            return;
        setup.useDefaultInputChannels = false;
        setup.inputChannels = ins;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    void refreshChannelBoxes()
    {
        auto* dev = deviceManager.getCurrentAudioDevice();
        const auto ins = dev ? dev->getInputChannelNames() : StringArray();
        auto nameOf = [] (const StringArray& a, int i) { return String (i + 1) + (a[i].isNotEmpty() && a[i] != String (i + 1) ? " (" + a[i] + ")" : String()); };

        masterBox.clear (dontSendNotification);
        for (int i = 0; i < jmax (1, ins.size()); i += 2)
            masterBox.addItem (i + 1 < ins.size() ? "Entradas " + String (i + 1) + "-" + String (i + 2) : "Entrada " + String (i + 1), i / 2 + 1);
        masterBox.setSelectedId (jlimit (1, jmax (1, masterBox.getNumItems()), masterStart.load() / 2 + 1), dontSendNotification);

        // the mic: another input of this device, or any other input device
        // (Windows often shows each input pair of an interface as its own device)
        micBox.clear (dontSendNotification);
        micBox.addItem ("Desligado", 1);
        for (int i = 0; i < ins.size(); ++i) micBox.addItem ("Entrada " + nameOf (ins, i), i + 2);
        micDevices.clear();
        if (auto* t = micIn.getCurrentDeviceTypeObject()) micDevices = t->getDeviceNames (true);
        if (micDevices.size() > 0) micBox.addSectionHeading ("Outros dispositivos");
        for (int d = 0; d < micDevices.size(); ++d)
            for (int c = 0; c < 2; ++c)
                micBox.addItem (micDevices[d] + " · canal " + String (c + 1), 1000 + d * 2 + c);
        int sel = 1;
        if (micChannel.load() >= 0 && micChannel.load() < ins.size()) sel = micChannel.load() + 2;
        else if (micDevName.isNotEmpty() && micDevices.contains (micDevName)) sel = 1000 + micDevices.indexOf (micDevName) * 2 + micDevCh.load();
        micBox.setSelectedId (sel, dontSendNotification);

        outBox.clear (dontSendNotification);
        outBox.addItem ("Desligado", 1);
        StringArray outs;
        if (auto* t = voiceOut.getCurrentDeviceTypeObject()) outs = t->getDeviceNames (false);
        for (int i = 0; i < outs.size(); ++i) outBox.addItem (outs[i], i + 2);
        const int outSel = outs.indexOf (talkOutName);
        outBox.setSelectedId (outSel >= 0 ? outSel + 2 : 1, dontSendNotification);
        updateTalkEnabled();
    }

    void setMaster (int start, bool manual)
    {
        masterStart = start;
        props->setValue ("masterStart", start);
        if (manual) props->setValue ("masterManual", true);
        masterBox.setSelectedId (start / 2 + 1, dontSendNotification);
        trace (String (manual ? "master: chosen " : "master: auto ") + "inputs " + String (start + 1) + "-" + String (start + 2));
        updateStreamFormat();
    }

    // Signal on some other pair but not on the master's? Follow it (unless he chose the
    // master pair by hand), or say where the sound is.
    String findSignal()
    {
        const auto now = Time::getMillisecondCounter();
        int bestPair = -1; float best = 0;
        for (int c = 0; c < jmin ((int) devIns, kMaxCh); ++c) {
            const float pk = chanPeak[c].exchange (0.0f);
            if (pk > 0.0005f) chanLastMs[c] = now;
            if (c == micChannel.load()) continue;
            if (now - chanLastMs[c] < 1500 && pk >= best) { best = pk; bestPair = c & ~1; }
        }
        const int m = masterStart.load();
        const bool masterAlive = now - chanLastMs[m] < 3000 || (m + 1 < kMaxCh && now - chanLastMs[m + 1] < 3000);
        if (masterAlive || bestPair < 0 || bestPair == m) { silentSinceMs = now; return {}; }
        if (now - silentSinceMs < 2000) return {};
        if (! props->getBoolValue ("masterManual", false)) { setMaster (bestPair, false); return {}; }
        return u8 ("AO VIVO · sem sinal em ") + String (m + 1) + "-" + String (m + 2) + u8 (" · o som chega em ")
             + String (bestPair + 1) + "-" + String (bestPair + 2) + u8 (": troque em Master");
    }

    void updateStreamFormat()
    {
        if (auto* d = deviceManager.getCurrentAudioDevice())
            streamer.prepare (d->getCurrentSampleRate(), masterStart.load() + 1 < devIns ? 2 : 1);
    }

    void updateTalkEnabled()
    {
        const bool on = micChannel.load() >= 0 || micDevName.isNotEmpty() || talkOutName.isNotEmpty();
        if (on != streamer.isTalkbackEnabled()) trace (String ("talkback: ") + (on ? "on" : "off"));
        streamer.setTalkbackEnabled (on);
    }

    // ---- talkback mic from another device (shared mode, never touches the capture)
    void initMicInput()
    {
        micIn.initialise (2, 0, nullptr, false);
       #if JUCE_WINDOWS
        micIn.setCurrentAudioDeviceType ("Windows Audio", false);
       #endif
        if (auto* t = micIn.getCurrentDeviceTypeObject()) t->scanForDevices();
        micIn.closeAudioDevice();
        micIn.addAudioCallback (&micInCb);
        if (micChannel.load() < 0 && micDevName.isNotEmpty()) openMicDevice();
    }
    void openMicDevice()
    {
        auto setup = micIn.getAudioDeviceSetup();
        setup.outputDeviceName = {};
        setup.inputDeviceName = micDevName;
        setup.useDefaultInputChannels = true;
        const auto err = micIn.setAudioDeviceSetup (setup, true);
        trace ("talkback mic: " + micDevName + " ch " + String (micDevCh.load() + 1) + (err.isNotEmpty() ? "  ERROR " + err : String()));
        if (err.isNotEmpty()) micIn.closeAudioDevice();
    }
    void chooseMic (int id)
    {
        // stop feeding the mic lane before it is rebuilt
        micChannel = -1;
        micIn.closeAudioDevice();
        Thread::sleep (60);
        if (id >= 1000) {
            micDevName = micDevices[(id - 1000) / 2];
            micDevCh = (id - 1000) % 2;
            openMicDevice();
        } else {
            micDevName = {};
            if (id >= 2) {
                if (auto* d = deviceManager.getCurrentAudioDevice()) talkback.prepareMic (d->getCurrentSampleRate());
                micChannel = id - 2;
            } else talkback.stopMic();
        }
        props->setValue ("talkMic", micChannel.load());
        props->setValue ("talkMicDevice", micDevName);
        props->setValue ("talkMicCh", micDevCh.load());
        updateTalkEnabled();
    }
    struct MicInCallback : AudioIODeviceCallback
    {
        MicInCallback (SonoRocket::Talkback& t, std::atomic<int>& c) : tb (t), ch (c) {}
        void audioDeviceAboutToStart (AudioIODevice* d) override { tb.prepareMic (d->getCurrentSampleRate()); }
        void audioDeviceStopped() override {}
        void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                               const AudioIODeviceCallbackContext&) override
        {
            for (int c = 0; c < numOut; ++c) if (out[c]) FloatVectorOperations::clear (out[c], n);
            if (numIn <= 0) return;
            const int c = jlimit (0, numIn - 1, ch.load());
            if (in[c] != nullptr) tb.captureMic (in[c], n);
        }
        SonoRocket::Talkback& tb;
        std::atomic<int>& ch;
    };

    // ---- talkback output: its own device, shared mode, so it can never take the capture down
    void initVoiceOutput()
    {
        voiceOut.initialise (0, 2, nullptr, false);
       #if JUCE_WINDOWS
        voiceOut.setCurrentAudioDeviceType ("Windows Audio", false);   // shared mode: plays alongside anything
       #endif
        if (auto* t = voiceOut.getCurrentDeviceTypeObject()) t->scanForDevices();
        voiceOut.addAudioCallback (&voiceOutCb);
        openVoiceOutput();
    }
    void openVoiceOutput()
    {
        auto setup = voiceOut.getAudioDeviceSetup();
        setup.inputDeviceName = {};
        setup.outputDeviceName = talkOutName;
        setup.useDefaultOutputChannels = true;
        if (talkOutName.isEmpty()) { voiceOut.closeAudioDevice(); return; }
        const auto err = voiceOut.setAudioDeviceSetup (setup, true);
        trace ("talkback out: " + talkOutName + (err.isNotEmpty() ? "  ERROR " + err : String()));
    }
    struct VoiceOutCallback : AudioIODeviceCallback
    {
        explicit VoiceOutCallback (SonoRocket::Talkback& t) : tb (t) {}
        void audioDeviceAboutToStart (AudioIODevice* d) override { tb.prepareOutput (d->getCurrentSampleRate()); }
        void audioDeviceStopped() override {}
        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                               const AudioIODeviceCallbackContext&) override
        {
            if (numOut <= 0 || out[0] == nullptr) return;
            tb.render (out[0], n);
            for (int c = 1; c < numOut; ++c) if (out[c]) FloatVectorOperations::copy (out[c], out[0], n);
        }
        SonoRocket::Talkback& tb;
    };

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
        const bool haveMic = (micChannel.load() >= 0 && micChannel.load() < devIns)
                          || (micDevName.isNotEmpty() && micIn.getCurrentAudioDevice() != nullptr);
        talkback.setMicOpen (haveMic && streamer.getState() == State::Streaming && (! playing || ptt), ptt);

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
        else if (const auto hint = findSignal(); hint.isNotEmpty()) { s = hint; }
        else if (heldPeak[0] < 1e-6f && heldPeak[1] < 1e-6f) { s = u8 ("AO VIVO · sem sinal na entrada (dê play no Pro Tools)"); }
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
    std::atomic<int> masterStart { 0 }, micChannel { -1 };
    std::atomic<int> devIns { 0 };
    std::atomic<float> chanPeak[64] {};
    juce::uint32 chanLastMs[64] {};
    juce::uint32 silentSinceMs = 0;
    AudioDeviceManager voiceOut, micIn;
    VoiceOutCallback voiceOutCb { talkback };
    std::atomic<int> micDevCh { 0 };
    MicInCallback micInCb { talkback, micDevCh };
    String talkOutName, micDevName;
    StringArray micDevices;
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
    TextButton testBtn { "Testar" };
    HoldButton talkBtn { "Segure para falar (Ctrl+T)" };
    TextButton copyBtn { "Copiar link" }, whatsBtn { "WhatsApp" }, streamBtn { "Parar" }, videoBtn { u8 ("Vídeo") };
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
