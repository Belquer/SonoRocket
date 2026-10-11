// SonoRocket (capture app): streams any audio input, bit-exact, to any browser.
// For setups where a plugin can't go on the master (old Pro Tools / RTAS,
// a second "streaming" computer fed by cable, any app at all).
//
// Double-click → it starts its own relay + public tunnel (bundled next to the
// executable), opens the last-used audio input, and shows the link to share.

#include <juce_audio_utils/juce_audio_utils.h>
#include "../RocketStreamer.h"

#if JUCE_WINDOWS
 #include <windows.h>
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
        if (process->start (args, 0)) weStartedIt = true;
        else setError ("não consegui iniciar o relay");
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
class MainComponent : public Component, private Timer, private AudioIODeviceCallback, private ChangeListener
{
public:
    MainComponent()
    {
        PropertiesFile::Options o;
        o.applicationName = "SonoRocket"; o.folderName = "SonoRocket"; o.filenameSuffix = "settings";
        o.osxLibrarySubFolder = "Application Support";
        props = std::make_unique<PropertiesFile> (o);

        // stable room per computer: the listener link stays predictable
        if (props->getValue ("room").isEmpty())
            props->setValue ("room", "mix-" + String::toHexString (Random().nextInt64()).substring (0, 8));

        // --- audio input
        std::unique_ptr<XmlElement> saved (props->getXmlValue ("audio"));
        if (saved == nullptr) {
           #if JUCE_WINDOWS
            // exclusive mode bypasses the Windows mixer (no resampling, no effects)
            deviceManager.setCurrentAudioDeviceType ("Windows Audio (Exclusive Mode)", false);
           #endif
        }
        deviceManager.initialise (2, 0, saved.get(), true);
        deviceManager.addAudioCallback (this);
        deviceManager.addChangeListener (this);

        selector = std::make_unique<AudioDeviceSelectorComponent> (deviceManager, 1, 2, 0, 0, false, false, true, false);
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

        inputTitle.setText (u8 ("Entrada de áudio (por onde chega o som do Pro Tools)"), dontSendNotification);
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
        copyBtn.onClick = [this] {
            if (currentLink.isEmpty()) return;
            SystemClipboard::copyTextToClipboard (currentLink);
            copyBtn.setButtonText ("Copiado!");
            Timer::callAfterDelay (1500, [sp = SafePointer<MainComponent> (this)] { if (sp) sp->copyBtn.setButtonText ("Copiar link"); });
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

        setSize (560, 720);
        startTimerHz (10);
    }

    ~MainComponent() override
    {
        stopTimer();
        deviceManager.removeAudioCallback (this);
        deviceManager.removeChangeListener (this);
        saveAudioState();
        relay.stop();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (kBg);
        g.setColour (kPanel);
        g.fillRoundedRectangle (inputPanel.toFloat(), 12.0f);
        g.fillRoundedRectangle (sharePanel.toFloat(), 12.0f);
    }

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
        const int chans = jlimit (1, 2, d->getActiveInputChannels().countNumberOfSetBits());
        streamer.prepare (d->getCurrentSampleRate(), chans);
        sampleRate = d->getCurrentSampleRate();
    }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int n, const AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c) if (out[c]) FloatVectorOperations::clear (out[c], n);
        if (numIn <= 0 || n <= 0) return;

        const int chans = jmin (2, numIn);
        streamer.pushAudio (in, chans, n);   // raw samples, untouched

        for (int c = 0; c < chans; ++c) {
            auto r = FloatVectorOperations::findMinAndMax (in[c], n);
            const float pk = jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
            float prev = peak[c].load();
            while (pk > prev && ! peak[c].compare_exchange_weak (prev, pk)) {}
        }
        if (chans == 1) peak[1].store (peak[0].load());
    }

    void changeListenerCallback (ChangeBroadcaster*) override { saveAudioState(); }
    void saveAudioState()
    {
        if (auto xml = deviceManager.createStateXml()) props->setValue ("audio", xml.get());
        props->saveIfNeeded();
    }

    // ---- UI refresh
    void timerCallback() override
    {
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
        else if (heldPeak[0] < 1e-6f && heldPeak[1] < 1e-6f) { s = u8 ("AO VIVO · sem sinal na entrada"); }
        else                                        { s = "AO VIVO"; col = kGreen; }

        if (st == State::Streaming)
            d << String (sampleRate, 0) << " Hz · float32 · " << rs.listeners << (rs.listeners == 1 ? " ouvindo" : " ouvindo")
              << " · fila " << String (streamer.getFifoSeconds(), 2) << " s";
        if (streamer.getFramesDropped() > 0) d << " · PERDEU " << String (streamer.getFramesDropped());

        status.setText (u8 ("● ") + s, dontSendNotification);
        status.setColour (Label::textColourId, col);
        detail.setText (d, dontSendNotification);
        link.setText (currentLink.isNotEmpty() ? currentLink : u8 ("o link aparece aqui em alguns segundos"), dontSendNotification);
        link.setColour (Label::textColourId, currentLink.isNotEmpty() ? kText : kDim);
        copyBtn.setEnabled (currentLink.isNotEmpty());
        whatsBtn.setEnabled (currentLink.isNotEmpty());
    }

    std::unique_ptr<PropertiesFile> props;
    AudioDeviceManager deviceManager;
    std::unique_ptr<AudioDeviceSelectorComponent> selector;
    SonoRocket::RocketStreamer streamer;
    RelayManager relay;
    std::atomic<float> peak[2] { {0.0f}, {0.0f} };
    float heldPeak[2] { 0, 0 };
    double sampleRate = 48000;
    String currentLink;

    Label title, version, status, detail, inputTitle, labL, labR, dbL, dbR, link, tip;
    LevelMeter meterL, meterR;
    TextButton copyBtn { "Copiar link" }, whatsBtn { "Mandar no WhatsApp" }, streamBtn { "Parar" };
    Rectangle<int> inputPanel, sharePanel;

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
            setContentOwned (new MainComponent(), true);
            setResizable (true, false);
            setResizeLimits (480, 640, 900, 1200);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SonoRocketApp)
