// SonoRocket: settings + status panel for browser streaming.
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
    int getPreferredHeight() const { return 372; }

private:
    void timerCallback() override;
    void loadFromConfig();
    void applyToConfig (bool enabledOverride, bool enabledValue);
    String listenerLink() const;

    SonobusAudioProcessor& processor;

    Label title, hostLabel, portLabel, roomLabel, keyLabel, urlLabel, statusLabel, linkLabel, hint;
    TextEditor hostEdit, portEdit, roomEdit, keyEdit, urlEdit;
    TextButton newRoomButton { TRANS("New") };
    TextButton streamButton { TRANS("Start streaming") };
    TextButton copyButton { TRANS("Copy listener link") };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RocketView)
};
