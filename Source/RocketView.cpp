// SonoRocket: browser streaming panel.
#include "RocketView.h"

using State = SonoRocket::RocketStreamer::State;

static const Colour kOrange (0xffff5a1f);
static const Colour kGreen (0xff3ddc84);
static const Colour kRed (0xffff4d4d);
static const Colour kAmber (0xffffc23d);

RocketView::RocketView (SonobusAudioProcessor& p) : processor (p)
{
    title.setText ("SonoRocket", dontSendNotification);
    title.setFont (Font (22.0f, Font::bold));
    title.setColour (Label::textColourId, kOrange);
    addAndMakeVisible (title);

    statusLabel.setFont (Font (16.0f, Font::bold));
    addAndMakeVisible (statusLabel);

    linkLabel.setFont (Font (Font::getDefaultMonospacedFontName(), 13.0f, Font::plain));
    linkLabel.setColour (Label::textColourId, Colours::white);
    linkLabel.setColour (Label::backgroundColourId, Colour (0xff0b0b0c));
    linkLabel.setJustificationType (Justification::centredLeft);
    linkLabel.setBorderSize (BorderSize<int> (6, 10, 6, 10));
    addAndMakeVisible (linkLabel);

    detailLabel.setFont (Font (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
    detailLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.55f));
    addAndMakeVisible (detailLabel);

    copyButton.onClick = [this] {
        const auto link = listenerLink();
        if (link.isNotEmpty()) {
            SystemClipboard::copyTextToClipboard (link);
            copyButton.setButtonText ("Copiado!");
            Timer::callAfterDelay (1500, [safe = Component::SafePointer<RocketView> (this)] {
                if (safe) safe->copyButton.setButtonText ("Copiar link");
            });
        }
    };
    addAndMakeVisible (copyButton);

    whatsappButton.setColour (TextButton::buttonColourId, Colour (0xff25d366));
    whatsappButton.setColour (TextButton::textColourOffId, Colours::black);
    whatsappButton.onClick = [this] {
        const auto link = listenerLink();
        if (link.isEmpty()) return;
        const String msg = String::fromUTF8 ("Mix ao vivo, qualidade máxima: ") + link;
        URL ("https://wa.me/?text=" + URL::addEscapeChars (msg, true)).launchInDefaultBrowser();
    };
    addAndMakeVisible (whatsappButton);

    streamButton.setColour (TextButton::buttonColourId, kOrange);
    streamButton.setColour (TextButton::textColourOffId, Colours::black);
    streamButton.onClick = [this] { applyToConfig (true, ! processor.getRocket().getConfig().enabled); };
    addAndMakeVisible (streamButton);

    advancedButton.setClickingTogglesState (true);
    advancedButton.onClick = [this] { setAdvancedVisible (advancedButton.getToggleState()); };
    addAndMakeVisible (advancedButton);

    auto setupField = [this] (Label& l, const String& text, TextEditor& e) {
        l.setText (text, dontSendNotification);
        l.setFont (Font (13.0f));
        l.setJustificationType (Justification::centredRight);
        addChildComponent (l);
        e.setFont (Font (Font::getDefaultMonospacedFontName(), 14.0f, Font::plain));
        e.onReturnKey = [this] { applyToConfig (false, false); };
        e.onFocusLost = [this] { applyToConfig (false, false); };
        addChildComponent (e);
    };
    setupField (hostLabel, "Relay", hostEdit);
    setupField (portLabel, "Porta", portEdit);
    setupField (roomLabel, "Sala", roomEdit);
    setupField (keyLabel, "Chave", keyEdit);
    setupField (urlLabel, "URL pública", urlEdit);
    portEdit.setInputRestrictions (5, "0123456789");
    roomEdit.setInputRestrictions (64, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
    roomEdit.setTextToShowWhenEmpty ("auto", Colours::grey);
    keyEdit.setPasswordCharacter ((juce_wchar) 0x2022);
    urlEdit.setTextToShowWhenEmpty (String::fromUTF8 ("automático (vem do app)"), Colours::grey);

    loadFromConfig();
    timerCallback();
    startTimerHz (4);
}

RocketView::~RocketView()
{
    applyToConfig (false, false);
}

void RocketView::setAdvancedVisible (bool show)
{
    showAdvanced = show;
    for (Component* c : { (Component*) &hostLabel, (Component*) &portLabel, (Component*) &roomLabel, (Component*) &keyLabel, (Component*) &urlLabel,
                          (Component*) &hostEdit, (Component*) &portEdit, (Component*) &roomEdit, (Component*) &keyEdit, (Component*) &urlEdit })
        c->setVisible (show);
    if (onPreferredHeightChanged) onPreferredHeightChanged();
    resized();
}

void RocketView::loadFromConfig()
{
    const auto c = processor.getRocket().getConfig();
    hostEdit.setText (c.host, dontSendNotification);
    portEdit.setText (String (c.port), dontSendNotification);
    roomEdit.setText (c.room == "auto" ? String() : c.room, dontSendNotification);
    keyEdit.setText (c.key, dontSendNotification);
    urlEdit.setText (c.listenUrl, dontSendNotification);
}

void RocketView::applyToConfig (bool enabledOverride, bool enabledValue)
{
    auto& rocket = processor.getRocket();
    const auto c = rocket.getConfig();
    auto n = c;
    n.host = hostEdit.getText().trim().isEmpty() ? String ("127.0.0.1") : hostEdit.getText().trim();
    n.port = jlimit (1, 65535, portEdit.getText().getIntValue() > 0 ? portEdit.getText().getIntValue() : 9100);
    n.room = roomEdit.getText().trim().isEmpty() ? String ("auto") : roomEdit.getText().trim();
    n.key = keyEdit.getText();
    n.listenUrl = urlEdit.getText().trim().trimCharactersAtEnd ("/");
    if (enabledOverride) n.enabled = enabledValue;

    if (n.host != c.host || n.port != c.port || n.room != c.room || n.key != c.key
        || n.enabled != c.enabled || n.listenUrl != c.listenUrl)
        rocket.setConfig (n);
}

String RocketView::listenerLink() const
{
    auto& r = processor.getRocket();
    const auto c = r.getConfig();
    // a manually set public URL wins; otherwise use what the relay reported
    if (c.listenUrl.isNotEmpty() && c.room != "auto")
        return c.listenUrl + "/?room=" + URL::addEscapeChars (c.room, true);
    return r.getListenerLink();
}

void RocketView::timerCallback()
{
    auto& r = processor.getRocket();
    const auto c = r.getConfig();
    const auto st = r.getState();
    const auto link = listenerLink();
    const bool publicLink = link.startsWith ("https://");

    streamButton.setButtonText (c.enabled ? "Parar" : "Transmitir");

    String status, detail;
    if (! c.enabled)                   { status = "Desligado"; statusColour = Colours::white.withAlpha (0.5f); }
    else if (st == State::Streaming)   {
        status = publicLink ? "AO VIVO" : String::fromUTF8 ("AO VIVO · gerando link público…");
        statusColour = publicLink ? kGreen : kAmber;
        detail << String (processor.getSampleRate(), 0) << " Hz · float32 · fila " << String (r.getFifoSeconds(), 2) << " s";
        if (r.getFramesDropped() > 0) detail << " · PERDEU " << String (r.getFramesDropped()) << " frames";
    }
    else if (st == State::Error && r.getStatusText().contains ("can't reach")) {
        status = String::fromUTF8 ("Abra o app SonoRocket");
        statusColour = kAmber;
        detail = String::fromUTF8 ("o plugin conecta sozinho quando o app abrir");
    }
    else if (st == State::Error)       { status = "Erro"; statusColour = kRed; detail = r.getStatusText(); }
    else                               { status = "Conectando..."; statusColour = kAmber; }

    statusLabel.setText (String::fromUTF8 ("● ") + status, dontSendNotification);
    statusLabel.setColour (Label::textColourId, statusColour);
    detailLabel.setText (detail, dontSendNotification);
    linkLabel.setText (link.isNotEmpty() ? link : String::fromUTF8 ("o link aparece aqui"), dontSendNotification);
    linkLabel.setColour (Label::textColourId, link.isNotEmpty() ? Colours::white : Colours::white.withAlpha (0.4f));
    copyButton.setEnabled (link.isNotEmpty());
    whatsappButton.setEnabled (publicLink);
}

void RocketView::paint (Graphics& g)
{
    g.fillAll (Colour (0xff151517));
}

void RocketView::resized()
{
    auto b = getLocalBounds().reduced (14);
    auto top = b.removeFromTop (30);
    advancedButton.setBounds (top.removeFromRight (90).reduced (0, 3));
    title.setBounds (top);
    b.removeFromTop (6);
    statusLabel.setBounds (b.removeFromTop (24));
    detailLabel.setBounds (b.removeFromTop (18));
    b.removeFromTop (8);
    linkLabel.setBounds (b.removeFromTop (32));
    b.removeFromTop (8);
    {
        auto row = b.removeFromTop (36);
        copyButton.setBounds (row.removeFromLeft (row.getWidth() / 2 - 4));
        row.removeFromLeft (8);
        whatsappButton.setBounds (row);
    }
    b.removeFromTop (8);
    streamButton.setBounds (b.removeFromTop (40));

    if (showAdvanced) {
        b.removeFromTop (14);
        auto field = [&] (Label& l, TextEditor& e) {
            auto r = b.removeFromTop (30);
            l.setBounds (r.removeFromLeft (90));
            r.removeFromLeft (6);
            e.setBounds (r);
            b.removeFromTop (6);
        };
        {
            auto r = b.removeFromTop (30);
            hostLabel.setBounds (r.removeFromLeft (90));
            r.removeFromLeft (6);
            portEdit.setBounds (r.removeFromRight (70));
            portLabel.setBounds (r.removeFromRight (48));
            r.removeFromRight (4);
            hostEdit.setBounds (r);
            b.removeFromTop (6);
        }
        field (roomLabel, roomEdit);
        field (keyLabel, keyEdit);
        field (urlLabel, urlEdit);
    }
}
