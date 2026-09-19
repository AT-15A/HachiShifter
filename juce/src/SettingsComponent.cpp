#include "SettingsComponent.h"
#include "backend/NsfHifiganRenderer.h"
#include <cmath>

namespace hachi
{
SettingsComponent::PathPicker::PathPicker()
{
    addAndMakeVisible(editor);
    addAndMakeVisible(browseButton);
    editor.setSelectAllWhenFocused(true);
    browseButton.onClick = [this]
    {
        if (onBrowse) onBrowse();
    };
}

void SettingsComponent::PathPicker::resized()
{
    auto area = getLocalBounds();
    auto buttonArea = area.removeFromRight(38);
    area.removeFromRight(5);
    editor.setBounds(area);
    browseButton.setBounds(buttonArea);
}

void SettingsComponent::PathPicker::setText(const juce::String& text)
{
    editor.setText(text, false);
}

juce::String SettingsComponent::PathPicker::getText() const
{
    return editor.getText().trim();
}

void SettingsComponent::PathPicker::setBrowseTooltip(const juce::String& text)
{
    browseButton.setTooltip(text);
}

void SettingsComponent::FormPage::addRow(juce::Label& label, juce::Component& editor)
{
    addAndMakeVisible(label);
    addAndMakeVisible(editor);
    rows.push_back({ &label, &editor, 30 });
}

void SettingsComponent::FormPage::addWide(juce::Component& component, int height)
{
    addAndMakeVisible(component);
    rows.push_back({ nullptr, &component, height });
}

void SettingsComponent::FormPage::resized()
{
    auto area = getLocalBounds().reduced(18, 14);
    for (const auto& row : rows)
    {
        auto line = area.removeFromTop(row.height);
        area.removeFromTop(7);
        if (row.label != nullptr)
        {
            row.label->setBounds(line.removeFromLeft(210));
            line.removeFromLeft(10);
        }
        row.editor->setBounds(line);
    }
}

SettingsComponent::SettingsComponent(I18n& stringsToUse,
                                     juce::AudioDeviceManager& devicesToUse,
                                     juce::PropertiesFile& propertiesToUse,
                                     Applied appliedToUse)
    : strings(stringsToUse), devices(devicesToUse), properties(propertiesToUse),
      onApplied(std::move(appliedToUse))
{
    addAndMakeVisible(tabs);
    addAndMakeVisible(applyButton);

    language.addItem("zh-CN", 1);
    language.addItem("zh-TW", 2);
    language.addItem("ja-JP", 3);
    language.addItem("ko-KR", 4);
    language.addItem("en-US", 5);
    theme.addItem("Dark", 1);
    theme.addItem("Light", 2);
    interfacePage.addRow(languageLabel, language);
    interfacePage.addRow(themeLabel, theme);
    interfacePage.addRow(accentLabel, accent);
    interfacePage.addRow(accentLightLabel, accentLight);
    interfacePage.addRow(noteColourLabel, noteColour);
    interfacePage.addWide(showNoteLabels);
    uiScale.setRange(0.6, 2.0, 0.1);
    uiScale.setSliderStyle(juce::Slider::LinearHorizontal);
    uiScale.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    uiScale.setValue(1.0);
    interfacePage.addRow(uiScaleLabel, uiScale);

    currentAudioDevice.setColour(juce::Label::backgroundColourId, Palette::background);
    currentAudioDevice.setColour(juce::Label::outlineColourId, Palette::border);
    currentAudioDevice.setJustificationType(juce::Justification::centredLeft);
    sampleRate.setEditableText(true);
    bufferSize.setEditableText(true);
    for (const auto value : { 32000, 44100, 48000, 88200, 96000, 192000 })
        sampleRate.addItem(juce::String(value) + " Hz", sampleRate.getNumItems() + 1);
    for (const auto value : { 32, 64, 128, 256, 512, 1024, 2048, 4096 })
        bufferSize.addItem(juce::String(value), bufferSize.getNumItems() + 1);
    audioPage.addRow(audioDeviceLabel, currentAudioDevice);
    audioPage.addRow(sampleRateLabel, sampleRate);
    audioPage.addRow(bufferSizeLabel, bufferSize);
    audioPage.addWide(advancedAudio);
    advancedAudio.onClick = [this] { openAdvancedAudioPanel(); };

    gameModel.addItem("large", 1);
    gameModel.addItem("small", 2);
    inference.addItem("Auto", 1);
    inference.addItem("CPU", 2);
    inference.addItem("DirectML", 3);
    inference.addItem("CUDA", 4);
    inference.addItem("CoreML", 5);
    inferenceDevice.addItem("Auto", 1);
    for (int device = 0; device < 8; ++device)
        inferenceDevice.addItem("GPU " + juce::String(device), device + 2);
    const auto refreshInferenceDevice = [this]
    {
        const auto selected = inference.getSelectedId();
        inferenceDevice.setEnabled(selected == 3 || selected == 4);
    };
    inference.onChange = refreshInferenceDevice;
    algorithmPage.addRow(gamePathLabel, gamePath);
    algorithmPage.addRow(gameModelLabel, gameModel);
    algorithmPage.addRow(fcpePathLabel, fcpePath);
    algorithmPage.addRow(hifiganPathLabel, hifiganPath);
    algorithmPage.addRow(inferenceLabel, inference);
    algorithmPage.addRow(inferenceDeviceLabel, inferenceDevice);
    algorithmPage.addRow(utauVoicebankLabel, utauVoicebankPath);
    algorithmPage.addRow(utauWavtoolLabel, utauWavtoolPath);
    algorithmPage.addRow(utauResamplerLabel, utauResamplerPath);
    utauVoicebankPath.onBrowse = [this] { chooseUtauVoicebank(); };
    utauWavtoolPath.onBrowse = [this] { chooseUtauWavtool(); };
    utauResamplerPath.onBrowse = [this] { chooseUtauResampler(); };

    shortcutPreset.addItem("HachiShifter", 1);
    shortcutPreset.addItem("Melodyne", 2);
    shortcutPreset.addItem("UTAU", 3);
    // The wheel-action choice is gone: the wheel scrolls and ctrl zooms, so
    // there is no longer a decision to offer.  Leaving the box would be a
    // control that changes nothing, which is worse than one less setting.
    operationPage.addRow(shortcutLabel, shortcutPreset);
    operationPage.addWide(spacePlayback);
    operationPage.addWide(confirmDestructive);

    melodyneCompose.addItem("Ask", 1);
    melodyneCompose.addItem("Melodic tracks", 2);
    melodyneCompose.addItem("All tracks", 3);
    melodyneCompose.addItem("Audio only", 4);
    melodynePitchSource.addItem("Project data", 1);
    melodynePitchSource.addItem("GAME + FCPE / Native fallback", 2);
    importedAlgorithm.addItem("nsf-hifigan", 2);
    importedAlgorithm.addItem("WORLD", 3);
    importedAlgorithm.addItem("llsm2", 6);
    const auto configuredHifigan = juce::File(properties.getValue("algorithm.hifiganPath"));
    const auto defaultImportedAlgorithm = backend::NsfHifiganRenderer::modelAvailable(
        configuredHifigan) ? 2 : 6;
    refreshImportedStretchItems(defaultImportedAlgorithm);
    importedAlgorithm.onChange = [this]
    {
        refreshImportedStretchItems(importedStretchAlgorithm.getSelectedId());
    };
    importPage.addRow(melodyneComposeLabel, melodyneCompose);
    importPage.addRow(melodynePitchLabel, melodynePitchSource);
    importPage.addRow(importedAlgorithmLabel, importedAlgorithm);
    importPage.addRow(importedStretchAlgorithmLabel, importedStretchAlgorithm);
    importPage.addWide(preserveProjectEdits);
    importPage.addWide(locateMediaRecursively);

    // JUCE does not create a tab button for an empty caption on every backend.
    // Adding empty names and renaming them later therefore produced a valid
    // dialog shell with zero pages.  Seed translated names immediately.
    tabs.addTab(strings.text("settings.interface"), Palette::panel, &interfacePage, false);
    tabs.addTab(strings.text("settings.audio"), Palette::panel, &audioPage, false);
    tabs.addTab(strings.text("settings.algorithm"), Palette::panel, &algorithmPage, false);
    tabs.addTab(strings.text("settings.operation"), Palette::panel, &operationPage, false);
    tabs.addTab(strings.text("settings.import"), Palette::panel, &importPage, false);
    // Select a concrete page before the dialog is attached to a peer.  On
    // headless/device-delayed startup JUCE may otherwise keep index -1 until a
    // tab click, making the settings dialog appear completely empty.
    tabs.setCurrentTabIndex(0, false);
    loadValues();
    refreshInferenceDevice();
    setTexts();
    applyButton.onClick = [this]
    {
        saveValues();
        setTexts();
        if (onApplied) onApplied();
    };
    setSize(720, 570);
}

SettingsComponent::~SettingsComponent()
{
    saveValues();
}

juce::Colour SettingsComponent::colourFromText(const juce::String& value,
                                               juce::Colour fallback)
{
    auto text = value.trim().removeCharacters("#");
    if (text.length() != 6 && text.length() != 8) return fallback;
    if (text.length() == 6) text = "ff" + text;
    return juce::Colour::fromString(text);
}

void SettingsComponent::loadValues()
{
    language.setSelectedId(properties.getIntValue("ui.language",
        static_cast<int>(strings.getLanguage()) + 1), juce::dontSendNotification);
    theme.setSelectedId(properties.getValue("ui.theme", "dark") == "light" ? 2 : 1,
                        juce::dontSendNotification);
    accent.setText(properties.getValue("ui.accent", "7F69CA"), false);
    accentLight.setText(properties.getValue("ui.accentLight", "CBCBFA"), false);
    noteColour.setText(properties.getValue("ui.noteColour", "F4C000"), false);
    showNoteLabels.setToggleState(properties.getBoolValue("ui.showNoteLabels", false),
                                  juce::dontSendNotification);
    uiScale.setValue(properties.getDoubleValue("ui.uiScale", 1.0), juce::dontSendNotification);
    gamePath.setText(properties.getValue("algorithm.gamePath"), false);
    gameModel.setSelectedId(properties.getValue("algorithm.gameModel", "large") == "small" ? 2 : 1,
                            juce::dontSendNotification);
    fcpePath.setText(properties.getValue("algorithm.fcpePath"), false);
    hifiganPath.setText(properties.getValue("algorithm.hifiganPath"), false);
    inference.setSelectedId(properties.getIntValue("algorithm.inference", 1), juce::dontSendNotification);
    inferenceDevice.setSelectedId(properties.getIntValue("algorithm.device", 1), juce::dontSendNotification);
    utauVoicebankPath.setText(properties.getValue("algorithm.utauVoicebank"));
    utauWavtoolPath.setText(properties.getValue("algorithm.utauWavtool"));
    utauResamplerPath.setText(properties.getValue("algorithm.utauResampler"));
    shortcutPreset.setSelectedId(properties.getIntValue("operation.shortcutPreset", 1), juce::dontSendNotification);
    spacePlayback.setToggleState(properties.getBoolValue("operation.spacePlayback", true), juce::dontSendNotification);
    confirmDestructive.setToggleState(properties.getBoolValue("operation.confirmDestructive", true), juce::dontSendNotification);
    melodyneCompose.setSelectedId(properties.getIntValue("import.melodyneCompose", 1), juce::dontSendNotification);
    melodynePitchSource.setSelectedId(properties.getIntValue("import.melodynePitchSource", 1), juce::dontSendNotification);
    const auto configuredHifigan = juce::File(properties.getValue("algorithm.hifiganPath"));
    const auto defaultImportedAlgorithm = backend::NsfHifiganRenderer::modelAvailable(
        configuredHifigan) ? 2 : 6;
    const auto importedAlgorithmId = properties.getIntValue("import.algorithm",
        defaultImportedAlgorithm);
    importedAlgorithm.setSelectedId(importedAlgorithm.indexOfItemId(importedAlgorithmId) >= 0
        ? importedAlgorithmId : defaultImportedAlgorithm, juce::dontSendNotification);
    refreshImportedStretchItems(properties.getIntValue("import.stretchAlgorithm", 1));
    preserveProjectEdits.setToggleState(properties.getBoolValue("import.preserveEdits", true), juce::dontSendNotification);
    locateMediaRecursively.setToggleState(properties.getBoolValue("import.recursiveMedia", true), juce::dontSendNotification);
    refreshAudioValues();
}

void SettingsComponent::saveValues()
{
    applyAudioValues();
    properties.setValue("ui.language", language.getSelectedId());
    properties.setValue("ui.theme", theme.getSelectedId() == 2 ? "light" : "dark");
    properties.setValue("ui.accent", accent.getText().trim());
    properties.setValue("ui.accentLight", accentLight.getText().trim());
    properties.setValue("ui.noteColour", noteColour.getText().trim());
    properties.setValue("ui.showNoteLabels", showNoteLabels.getToggleState());
    properties.setValue("ui.uiScale", uiScale.getValue());
    properties.setValue("algorithm.gamePath", gamePath.getText());
    properties.setValue("algorithm.gameModel", gameModel.getSelectedId() == 2 ? "small" : "large");
    properties.setValue("algorithm.fcpePath", fcpePath.getText());
    properties.setValue("algorithm.hifiganPath", hifiganPath.getText());
    properties.setValue("algorithm.inference", inference.getSelectedId());
    properties.setValue("algorithm.device", inferenceDevice.getSelectedId());
    properties.setValue("algorithm.utauVoicebank", utauVoicebankPath.getText());
    properties.setValue("algorithm.utauWavtool", utauWavtoolPath.getText());
    properties.setValue("algorithm.utauResampler", utauResamplerPath.getText());
    properties.setValue("operation.shortcutPreset", shortcutPreset.getSelectedId());
    properties.setValue("operation.spacePlayback", spacePlayback.getToggleState());
    properties.setValue("operation.confirmDestructive", confirmDestructive.getToggleState());
    properties.setValue("import.melodyneCompose", melodyneCompose.getSelectedId());
    properties.setValue("import.melodynePitchSource", melodynePitchSource.getSelectedId());
    properties.setValue("import.algorithm", importedAlgorithm.getSelectedId() > 0
        ? importedAlgorithm.getSelectedId() : 1);
    properties.setValue("import.stretchAlgorithm", importedStretchAlgorithm.getSelectedId());
    properties.setValue("import.preserveEdits", preserveProjectEdits.getToggleState());
    properties.setValue("import.recursiveMedia", locateMediaRecursively.getToggleState());
    properties.saveIfNeeded();
    strings.setLanguage(static_cast<I18n::Language>(juce::jlimit(1, 5, language.getSelectedId()) - 1));
    Palette::applyTheme(theme.getSelectedId() == 2 ? "light" : "dark",
        colourFromText(accent.getText(), juce::Colour(0xff7f69ca)),
        colourFromText(accentLight.getText(), juce::Colour(0xffcbcbfa)),
        colourFromText(noteColour.getText(), juce::Colour(0xfff4c000)));
}

void SettingsComponent::setTexts()
{
    tabs.setTabName(0, strings.text("settings.interface"));
    tabs.setTabName(1, strings.text("settings.audio"));
    tabs.setTabName(2, strings.text("settings.algorithm"));
    tabs.setTabName(3, strings.text("settings.operation"));
    tabs.setTabName(4, strings.text("settings.import"));
    languageLabel.setText(strings.text("settings.language"), juce::dontSendNotification);
    themeLabel.setText(strings.text("settings.theme"), juce::dontSendNotification);
    accentLabel.setText(strings.text("settings.accent"), juce::dontSendNotification);
    accentLightLabel.setText(strings.text("settings.accentLight"), juce::dontSendNotification);
    noteColourLabel.setText(strings.text("settings.noteColour"), juce::dontSendNotification);
    showNoteLabels.setButtonText(strings.text("settings.showNoteLabels"));
    uiScaleLabel.setText(strings.text("settings.uiScale"), juce::dontSendNotification);
    theme.changeItemText(1, strings.text("settings.themeDark"));
    theme.changeItemText(2, strings.text("settings.themeLight"));
    audioDeviceLabel.setText(strings.text("settings.audioDevice"), juce::dontSendNotification);
    sampleRateLabel.setText(strings.text("settings.sampleRate"), juce::dontSendNotification);
    bufferSizeLabel.setText(strings.text("settings.bufferSize"), juce::dontSendNotification);
    advancedAudio.setButtonText(strings.text("settings.advancedAudio"));
    gamePathLabel.setText(strings.text("settings.gamePath"), juce::dontSendNotification);
    gameModelLabel.setText(strings.text("settings.gameModel"), juce::dontSendNotification);
    fcpePathLabel.setText(strings.text("settings.fcpePath"), juce::dontSendNotification);
    hifiganPathLabel.setText(strings.text("settings.hifiganPath"), juce::dontSendNotification);
    inferenceLabel.setText(strings.text("settings.inference"), juce::dontSendNotification);
    inferenceDeviceLabel.setText(strings.text("settings.device"), juce::dontSendNotification);
    inference.changeItemText(1, strings.text("settings.auto"));
    inferenceDevice.changeItemText(1, strings.text("settings.auto"));
    utauVoicebankLabel.setText(strings.text("settings.utauVoicebank"), juce::dontSendNotification);
    utauWavtoolLabel.setText(strings.text("settings.utauWavtool"), juce::dontSendNotification);
    utauResamplerLabel.setText(strings.text("settings.utauResampler"), juce::dontSendNotification);
    const auto browseText = strings.text("settings.browse");
    utauVoicebankPath.setBrowseTooltip(browseText);
    utauWavtoolPath.setBrowseTooltip(browseText);
    utauResamplerPath.setBrowseTooltip(browseText);
    shortcutLabel.setText(strings.text("settings.shortcuts"), juce::dontSendNotification);
    spacePlayback.setButtonText(strings.text("settings.spacePlayback"));
    confirmDestructive.setButtonText(strings.text("settings.confirmDestructive"));
    melodyneComposeLabel.setText(strings.text("settings.melodyneCompose"), juce::dontSendNotification);
    melodyneCompose.changeItemText(1, strings.text("settings.composeAsk"));
    melodyneCompose.changeItemText(2, strings.text("settings.composeMelodic"));
    melodyneCompose.changeItemText(3, strings.text("settings.composeAll"));
    melodyneCompose.changeItemText(4, strings.text("settings.composeAudio"));
    melodynePitchLabel.setText(strings.text("settings.melodynePitch"), juce::dontSendNotification);
    melodynePitchSource.changeItemText(1, strings.text("settings.pitchProject"));
    melodynePitchSource.changeItemText(2, strings.text("settings.pitchReanalyse"));
    importedAlgorithmLabel.setText(strings.text("settings.importAlgorithm"), juce::dontSendNotification);
    importedStretchAlgorithmLabel.setText(strings.text("settings.importStretchAlgorithm"),
                                           juce::dontSendNotification);
    refreshImportedStretchItems(importedStretchAlgorithm.getSelectedId());
    preserveProjectEdits.setButtonText(strings.text("settings.preserveEdits"));
    locateMediaRecursively.setButtonText(strings.text("settings.recursiveMedia"));
    applyButton.setButtonText(strings.text("dialog.apply"));
}

void SettingsComponent::refreshImportedStretchItems(int preferredId)
{
    const auto previous = preferredId > 0 ? preferredId
                                           : importedStretchAlgorithm.getSelectedId();
    importedStretchAlgorithm.clear(juce::dontSendNotification);
    importedStretchAlgorithm.addItem(strings.text("algo.stretch.melodyneHybrid"), 1);
    if (importedAlgorithm.getSelectedId() == 2)
    {
        importedStretchAlgorithm.addItem(strings.text("algo.stretch.nsfVariableMel"), 2);
        importedStretchAlgorithm.addItem(strings.text("algo.stretch.nsfShiftThenSplice"), 5);
    }
    importedStretchAlgorithm.addItem(strings.text("algo.stretch.loop"), 3);
    importedStretchAlgorithm.addItem(strings.text("algo.stretch.soundTouch"), 4);
    const auto canUsePreferred = (previous != 2 && previous != 5)
        || importedAlgorithm.getSelectedId() == 2;
    importedStretchAlgorithm.setSelectedId(canUsePreferred && previous > 0 ? previous : 1,
                                             juce::dontSendNotification);
}

juce::File SettingsComponent::initialPathFor(const PathPicker& picker,
                                             bool directory) const
{
    const juce::File selected(picker.getText());
    if (directory && selected.isDirectory()) return selected;
    if (!directory && selected.existsAsFile()) return selected;
    if (selected.getParentDirectory().isDirectory()) return selected.getParentDirectory();
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

void SettingsComponent::chooseUtauVoicebank()
{
    pathChooser = std::make_unique<juce::FileChooser>(
        strings.text("settings.chooseUtauVoicebank"),
        initialPathFor(utauVoicebankPath, true));
    juce::Component::SafePointer<SettingsComponent> safe(this);
    pathChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
        [safe](const juce::FileChooser& chooser)
        {
            if (safe == nullptr) return;
            const auto directory = chooser.getResult();
            if (directory.isDirectory()) safe->utauVoicebankPath.setText(directory.getFullPathName());
        });
}

void SettingsComponent::chooseUtauWavtool()
{
    pathChooser = std::make_unique<juce::FileChooser>(
        strings.text("settings.chooseUtauWavtool"),
        initialPathFor(utauWavtoolPath, false), "*.exe;*.bat;*.cmd");
    juce::Component::SafePointer<SettingsComponent> safe(this);
    pathChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser)
        {
            if (safe == nullptr) return;
            const auto file = chooser.getResult();
            if (file.existsAsFile()) safe->utauWavtoolPath.setText(file.getFullPathName());
        });
}

void SettingsComponent::chooseUtauResampler()
{
    pathChooser = std::make_unique<juce::FileChooser>(
        strings.text("settings.chooseUtauResampler"),
        initialPathFor(utauResamplerPath, false), "*.exe;*.bat;*.cmd");
    juce::Component::SafePointer<SettingsComponent> safe(this);
    pathChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser)
        {
            if (safe == nullptr) return;
            const auto file = chooser.getResult();
            if (file.existsAsFile()) safe->utauResamplerPath.setText(file.getFullPathName());
        });
}

void SettingsComponent::resized()
{
    auto area = getLocalBounds().reduced(8);
    auto footer = area.removeFromBottom(40);
    applyButton.setBounds(footer.removeFromRight(120).reduced(4));
    tabs.setBounds(area);
}

void SettingsComponent::refreshAudioValues()
{
    if (auto* device = devices.getCurrentAudioDevice())
    {
        currentAudioDevice.setText(device->getName(), juce::dontSendNotification);
        sampleRate.setText(juce::String(static_cast<int>(std::llround(device->getCurrentSampleRate())))
                           + " Hz", juce::dontSendNotification);
        bufferSize.setText(juce::String(device->getCurrentBufferSizeSamples()),
                           juce::dontSendNotification);
    }
    else
    {
        currentAudioDevice.setText(strings.text("settings.noAudioDevice"),
                                   juce::dontSendNotification);
        sampleRate.setText("48000 Hz", juce::dontSendNotification);
        bufferSize.setText("512", juce::dontSendNotification);
    }
}

void SettingsComponent::applyAudioValues()
{
    if (devices.getCurrentAudioDevice() == nullptr) return;
    auto setup = devices.getAudioDeviceSetup();
    const auto requestedRate = sampleRate.getText().retainCharacters("0123456789.")
        .getDoubleValue();
    const auto requestedBuffer = bufferSize.getText().retainCharacters("0123456789")
        .getIntValue();
    auto changed = false;
    if (requestedRate > 0.0 && std::abs(setup.sampleRate - requestedRate) > 0.5)
    {
        setup.sampleRate = requestedRate;
        changed = true;
    }
    if (requestedBuffer > 0 && setup.bufferSize != requestedBuffer)
    {
        setup.bufferSize = requestedBuffer;
        changed = true;
    }
    if (changed)
    {
        const auto error = devices.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                strings.text("settings.audio"), error);
    }
    refreshAudioValues();
}

void SettingsComponent::openAdvancedAudioPanel()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(
        devices, 0, 2, 1, 2, true, true, true, false);
    selector->setSize(680, 500);
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = strings.text("settings.advancedAudio");
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.content.setOwned(selector);
    options.launchAsync();
}
}
