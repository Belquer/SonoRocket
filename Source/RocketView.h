// SonoRocket: browser streaming panel. Simple by default (status, link, share),
// with the technical fields tucked under "Avançado".
#pragma once

#include <JuceHeader.h>
#include "SonobusPluginProcessor.h"

class RocketView : public Component, private Timer
{
public:
    explicit RocketView (SonobusAudioProcessor& p);
    ~RocketView() override;

    void resized() override;
    void paint (Graphics& g) override;
    int getPreferredHeight() const { return showAdvanced ? 470 : 268; }

    std::function<void()> onPreferredHeightChanged;

private:
    void timerCallback() override;
    void loadFromConfig();
    void applyToConfig (bool enabledOverride, bool enabledValue);
    String listenerLink() const;
    void setAdvancedVisible (bool show);

    SonobusAudioProcessor& processor;
    bool showAdvanced = false;
    Colour statusColour { Colours::grey };

    Label title, statusLabel, linkLabel, detailLabel;
    TextButton copyButton { "Copiar link" };
    TextButton whatsappButton { "Mandar no WhatsApp" };
    TextButton streamButton { "Parar" };
    TextButton advancedButton { "Avançado" };

    // advanced
    Label hostLabel, portLabel, roomLabel, keyLabel, urlLabel;
    TextEditor hostEdit, portEdit, roomEdit, keyEdit, urlEdit;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RocketView)
};
