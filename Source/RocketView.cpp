// SonoRocket: settings + status panel for browser streaming.
#include "RocketView.h"

using State = SonoRocket::RocketStreamer::State;

static const Colour kRocketOrange (0xffff5a1f);

RocketView::RocketView (SonobusAudioProcessor& p) : processor (p)
{
    title.setText ("SonoRocket", dontSendNotification);
    title.setFont (Font (20.0f, Font::bold));
    title.setColour (Label::textColourId, kRocketOrange);
    addAndMakeVisible (title);

    hint.setText (TRANS("Streams this plugin's input (your master), bit-exact, to any browser."), dontSendNotification);
    hint.setFont (Font (13.0f));
    hint.setColour (Label::textColourId, Colours::white.withAlpha (0.6f));
    addAndMakeVisible (hint);

    auto setupField = [this] (Label& l, const String& text, TextEditor& e) {
        l.setText (text, dontSendNotification);
        l.setFont (Font (13.0f));
        l.setJustificationType (Justification::centredRight);
        addAndMakeVisible (l);
        e.setFont (Font (Font::getDefaultMonospacedFontName(), 14.0f, Font::plain));
        e.onReturnKey = [this] { applyToConfig (false, false); };
        e.onFocusLost = [this] { applyToConfig (false, false); };
        addAndMakeVisible (e);
    };
    setupField (hostLabel, TRANS("Relay"), hostEdit);
    setupField (portLabel, TRANS("Port"), portEdit);
    setupField (roomLabel, TRANS("Room"), roomEdit);
    setupField (keyLabel, TRANS("Key"), keyEdit);
    setupField (urlLabel, TRANS("Listen URL"), urlEdit);
    portEdit.setInputRestrictions (5, "0123456789");
    roomEdit.setInputRestrictions (64, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
    keyEdit.setPasswordCharacter ((juce_wchar) 0x2022);
    urlEdit.setTextToShowWhenEmpty ("https://your-relay.example", Colours::grey);

    newRoomButton.onClick = [this] {
        // unguessable room names: the link is the only "password" a listener needs
        const char* words[] = { "mix", "master", "take", "bounce", "groove", "verso", "ponte", "coro" };
        Random r;
        roomEdit.setText (String (words[r.nextInt (8)]) + "-" + String::toHexString (r.nextInt64()).substring (0, 10), dontSendNotification);
        applyToConfig (false, false);
    };
    addAndMakeVisible (newRoomButton);

    streamButton.setColour (TextButton::buttonColourId, kRocketOrange);
    streamButton.setColour (TextButton::textColourOffId, Colours::black);
    streamButton.onClick = [this] {
        applyToConfig (true, ! processor.getRocket().getConfig().enabled);
    };
    addAndMakeVisible (streamButton);

    copyButton.onClick = [this] {
        const auto link = listenerLink();
        if (link.isNotEmpty()) SystemClipboard::copyTextToClipboard (link);
    };
    addAndMakeVisible (copyButton);

    statusLabel.setFont (Font (Font::getDefaultMonospacedFontName(), 13.0f, Font::plain));
    addAndMakeVisible (statusLabel);
    linkLabel.setFont (Font (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
    linkLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.7f));
    addAndMakeVisible (linkLabel);

    loadFromConfig();
    timerCallback();
    startTimerHz (5);
}

RocketView::~RocketView()
{
    applyToConfig (false, false);
}

void RocketView::loadFromConfig()
{
    const auto c = processor.getRocket().getConfig();
    hostEdit.setText (c.host, dontSendNotification);
    portEdit.setText (String (c.port), dontSendNotification);
    roomEdit.setText (c.room, dontSendNotification);
    keyEdit.setText (c.key, dontSendNotification);
    urlEdit.setText (c.listenUrl, dontSendNotification);
}

void RocketView::applyToConfig (bool enabledOverride, bool enabledValue)
{
    auto& rocket = processor.getRocket();
    auto c = rocket.getConfig();
    auto n = c;
    n.host = hostEdit.getText().trim();
    n.port = jlimit (1, 65535, portEdit.getText().getIntValue() > 0 ? portEdit.getText().getIntValue() : 9100);
    n.room = roomEdit.getText().trim();
    n.key = keyEdit.getText();
    n.listenUrl = urlEdit.getText().trim().trimCharactersAtEnd ("/");
    if (enabledOverride) n.enabled = enabledValue;

    const bool changed = n.host != c.host || n.port != c.port || n.room != c.room || n.key != c.key
                      || n.enabled != c.enabled || n.listenUrl != c.listenUrl;
    if (changed) rocket.setConfig (n);
}

String RocketView::listenerLink() const
{
    const auto c = processor.getRocket().getConfig();
    if (c.room.isEmpty()) return {};
    String base = c.listenUrl;
    if (base.isEmpty()) base = "http://" + c.host + ":8080";
    return base + "/?room=" + URL::addEscapeChars (c.room, true);
}

void RocketView::timerCallback()
{
    auto& r = processor.getRocket();
    const auto c = r.getConfig();
    const auto st = r.getState();
    streamButton.setButtonText (c.enabled ? TRANS("Stop streaming") : TRANS("Start streaming"));

    String s;
    switch (st) {
        case State::Off:        s << "OFF"; break;
        case State::Connecting: s << "CONNECTING"; break;
        case State::Streaming:  s << "LIVE  " << String (processor.getSampleRate(), 0) << " Hz  float32"; break;
        case State::Error:      s << r.getStatusText(); break;
    }
    if (st == State::Streaming) {
        s << "\nqueue " << String (r.getFifoSeconds(), 2) << " s   sent "
          << String (r.getFramesSent() / jmax (1.0, processor.getSampleRate()), 0) << " s";
        if (r.getFramesDropped() > 0) s << "   DROPPED " << String (r.getFramesDropped()) << " frames";
    }
    statusLabel.setText (s, dontSendNotification);
    statusLabel.setColour (Label::textColourId,
                           st == State::Streaming ? Colour (0xff3ddc84)
                         : st == State::Error ? Colour (0xffff4d4d) : Colours::white.withAlpha (0.8f));
    linkLabel.setText (listenerLink(), dontSendNotification);
}

void RocketView::paint (Graphics& g)
{
    g.fillAll (Colour (0xff151517));
}

void RocketView::resized()
{
    auto b = getLocalBounds().reduced (12);
    title.setBounds (b.removeFromTop (28));
    hint.setBounds (b.removeFromTop (20));
    b.removeFromTop (8);

    auto row = [&] (Label& l, TextEditor& e, Component* extra = nullptr) {
        auto r = b.removeFromTop (30);
        l.setBounds (r.removeFromLeft (80));
        r.removeFromLeft (6);
        if (extra) { extra->setBounds (r.removeFromRight (60)); r.removeFromRight (6); }
        e.setBounds (r);
        b.removeFromTop (6);
    };
    {
        auto r = b.removeFromTop (30);
        hostLabel.setBounds (r.removeFromLeft (80));
        r.removeFromLeft (6);
        portEdit.setBounds (r.removeFromRight (70));
        portLabel.setBounds (r.removeFromRight (40));
        r.removeFromRight (4);
        hostEdit.setBounds (r);
        b.removeFromTop (6);
    }
    row (roomLabel, roomEdit, &newRoomButton);
    row (keyLabel, keyEdit);
    row (urlLabel, urlEdit);

    b.removeFromTop (4);
    streamButton.setBounds (b.removeFromTop (40));
    b.removeFromTop (8);
    statusLabel.setBounds (b.removeFromTop (40));
    linkLabel.setBounds (b.removeFromTop (22));
    b.removeFromTop (4);
    copyButton.setBounds (b.removeFromTop (30));
}
