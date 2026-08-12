#include <SKSEMenuSettings.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <unordered_map>

namespace MPL::SKSEMenuSettings
{
    namespace
    {
        const std::filesystem::path kSettingsPath{ "./Data/Luma/Tuning/skseMenuSettings.json" };

        struct StatusDisplay
        {
            std::string location = "bottom";
            double duration = 0.0;
            float fontScale = 1.0f;
            float height = 48.0f;
            std::optional<Color> color;
            bool clearOnPageChange = false;
        };

        struct ButtonFeedback
        {
            float hoverBrightness = 0.12f;
            float pressedBrightness = 0.35f;
        };

        struct ButtonColors
        {
            std::optional<Color> ordinary;
            std::optional<Color> save = Color{ 77.0f, 166.0f, 217.0f, 1.0f };
            std::optional<Color> restore;
            std::optional<Color> reset;
            std::optional<Color> destructive;
        };

        struct PresetColors
        {
            Color active{ 46.0f, 122.0f, 61.0f, 1.0f };
            Color activeHovered{ 71.0f, 148.0f, 87.0f, 1.0f };
            Color activePressed{ 120.0f, 168.0f, 133.0f, 1.0f };
            Color modified{ 133.0f, 41.0f, 41.0f, 1.0f };
            Color modifiedHovered{ 153.0f, 66.0f, 66.0f, 1.0f };
            Color modifiedPressed{ 176.0f, 115.0f, 115.0f, 1.0f };
        };

        struct Layout
        {
            float actionButtonSpacing = -1.0f;
            float pageTopSpacing = 0.0f;
            float sectionSpacing = 0.0f;
            float headerFontScale = 1.25f;
            std::array<float, 2> boxPadding{ 0.0f, 0.0f };
        };

        struct SettingsProfile
        {
            std::string position = "bottom";
            std::string label = "Settings";
            int blankLinesAbove = 1;
        };

        struct SettingLabels
        {
            std::string separator = " / ";
            std::unordered_map<std::string, std::string> groups;
            std::unordered_map<std::string, std::string> names;
            std::unordered_map<std::string, std::string> hues;
            std::unordered_map<std::string, std::string> settings;
        };

        struct Tooltip
        {
            std::string delay = "normal";
            float fontScale = 1.0f;
            std::optional<Color> textColor;
            std::optional<Color> backgroundColor;
        };

        struct ConfiguredSliderDefaults
        {
            float min = std::numeric_limits<float>::quiet_NaN();
            float max = std::numeric_limits<float>::quiet_NaN();
            float width = std::numeric_limits<float>::quiet_NaN();
            float step = std::numeric_limits<float>::quiet_NaN();
            std::string format;
        };

        struct Settings
        {
            bool enableTuningMenu = true;
            StatusDisplay statusDisplay;
            ButtonFeedback buttonFeedback;
            ButtonColors buttonColors;
            PresetColors presetColors;
            std::unordered_map<std::string, std::string> labels;
            SettingLabels settingLabels;
            Layout layout;
            std::unordered_map<std::string, ConfiguredSliderDefaults> sliderDefaults;
            SettingsProfile settingsProfile;
            Tooltip tooltip;
            std::unordered_map<std::string, std::string> statusMessages;
            std::unordered_map<std::string, std::string> displayMessages;
        };

        Settings settings;
        std::optional<std::filesystem::file_time_type> loadedWriteTime;
        std::chrono::steady_clock::time_point nextCheck{};
        bool initialized = false;

        const std::unordered_map<std::string, std::string>& DefaultStatusMessages()
        {
            static const std::unordered_map<std::string, std::string> messages{
                { "profileMenusReloaded", "Reloaded {count} profile menu(s)." },
                { "quickSelectSaveFailure", "Quick Select changed for this session, but its list could not be saved." },
                { "quickSelectClearSaveFailure", "Quick Select was cleared for this session, but its list could not be saved." },
                { "weatherLockEnabledSession", "Weather lock enabled for {weather} for this session." },
                { "weatherLockUnavailable", "Weather lock is enabled by this profile, but no current weather is available." },
                { "weatherLockDisabledReleaseRequested", "Weather lock disabled and the existing weather override was released." },
                { "profileStateSaveFailure", "The profile state could not be saved, so it was not changed." },
                { "profileEnabled", "Profile enabled and saved." },
                { "profileDisabled", "Profile disabled and saved." },
                { "settingOverrideChanged", "Changed locally, but {profile} overrides this setting." },
                { "presetSelectionSaved", "Preset selection saved." },
                { "presetSelectionSaveFailure", "The preset selection could not be saved. Check the TuningUtil log for details." },
                { "presetControlSaved", "Preset changes saved." },
                { "presetControlSaveFailure", "Preset changes could not be saved. Check the TuningUtil log for details." },
                { "presetControlRestored", "Preset changes restored." },
                { "presetDeselected", "Deselected the {category} preset." },
                { "presetPreview", "Previewing {category} preset {preset}." },
                { "presetLoadFailure", "The preset could not be loaded. Check the TuningUtil log for details." },
                { "presetCreated", "Saved preset {preset} in category {category}." },
                { "presetCreateFailure", "The preset could not be saved. Check the TuningUtil log for details." },
                { "presetRemovalStaged", "{preset} in {category} will be removed when Save Presets is selected." },
                { "presetCategoryRemovalStaged", "The {category} category will be removed when Save Presets is selected." },
                { "saveAllSuccess", "Saved all {profile} settings." },
                { "saveAllFailure", "Settings could not be saved. Check the TuningUtil log for details." },
                { "pageHasNoSettings", "This page has no settings that can be reset." },
                { "resetPageSuccess", "Default settings loaded for this page. Select Save Page to keep these changes." },
                { "resetPageFailure", "Page settings could not be reset. Check the TuningUtil log for details." },
                { "savePageSuccess", "Saved settings for this page." },
                { "savePageFailure", "Page settings could not be saved. Check the TuningUtil log for details." },
                { "restorePageSuccess", "Saved settings restored for this page." },
                { "restorePageFailure", "Page settings could not be restored. Check the TuningUtil log for details." },
                { "restoreAllSuccess", "Saved settings restored for this profile." },
                { "restoreAllFailure", "Profile settings could not be restored. Check the TuningUtil log for details." },
                { "resetAllSuccess", "Default settings loaded for this profile. Select Save All to keep these changes." },
                { "resetAllFailure", "Profile settings could not be reset. Check the TuningUtil log for details." },
                { "prioritySaved", "Profile priority applied and saved. The Luma menu order will use it on the next game launch." },
                { "prioritySaveFailure", "Profile priority applied for this session, but it could not be saved." },
                { "tuningMenuEnabled", "Tuning menus will be enabled after restarting Skyrim." },
                { "tuningMenuDisabled", "Tuning menus will be disabled after restarting Skyrim. Profiles will continue to apply at startup." },
                { "tuningMenuSaveFailure", "The tuning-menu setting could not be saved." },
                { "detailedLoggingEnabled", "Detailed logging enabled." },
                { "detailedLoggingDisabled", "Detailed logging disabled." },
                { "detailedLoggingSaveFailure", "The detailed-logging setting could not be saved." },
                { "userSettingsDeleted", "User settings and Quick Select lists deleted. Preset selections cleared." },
                { "userSettingsDeleteFailure", "Some user settings could not be deleted. Check the TuningUtil log for details." },
                { "userSettingsPromoted", "Made {profile} user settings permanent in profileSettings.json." },
                { "userSettingsPromoteFailure", "User settings could not be made permanent: {reason}" },
                { "sliderSaved", "Saved {slider} to {profile} / {page}." },
                { "sliderSaveFailure", "The slider could not be saved: {reason}" },
                { "profileCreated", "Created profile {profile}. Restart Skyrim to load its menu." },
                { "profileCreateFailure", "The profile could not be created: {reason}" },
                { "editModeEnabled", "Edit Mode enabled." },
                { "editModeDisabled", "Edit Mode disabled." },
                { "layoutPageSaved", "Page edits saved." },
                { "layoutPageRestored", "Saved page edits restored." },
                { "layoutPageSaveFailure", "Page edits could not be saved. Check the TuningUtil log for details." },
                { "layoutPageRestoreFailure", "Saved page edits could not be restored. Check the TuningUtil log for details." },
                { "layoutProfileSaved", "Profile edits saved." },
                { "layoutProfileRestored", "Saved profile edits restored." },
                { "layoutProfileSaveFailure", "Profile edits could not be saved. Check the TuningUtil log for details." },
                { "layoutProfileRestoreFailure", "Saved profile edits could not be restored. Check the TuningUtil log for details." },
                { "layoutAllSaved", "All profile edits saved." },
                { "layoutAllRestored", "Saved edits restored for all profiles." },
                { "layoutAllSaveFailure", "Some profile edits could not be saved. Check the TuningUtil log for details." },
                { "layoutAllRestoreFailure", "Some saved profile edits could not be restored. Check the TuningUtil log for details." },
                { "layoutEditSessionFailure", "Edit Mode could not create its temporary working layout. Check the TuningUtil log for details." },
                { "layoutPageAdded", "Page added." },
                { "layoutPageRenamed", "Page renamed." },
                { "layoutElementAdded", "Element added." },
                { "layoutPageMoved", "Page moved." },
                { "layoutPageRemoved", "Page removed." },
                { "layoutModuleAdded", "Module added." },
                { "layoutModuleMoved", "Module moved." },
                { "layoutModuleRemoved", "Module removed." },
            };
            return messages;
        }

        const std::unordered_map<std::string, std::string>& DefaultDisplayMessages()
        {
            static const std::unordered_map<std::string, std::string> messages{
                { "noSelectableWeathers", "No selectable weathers were found for this profile's loaded plugins." },
                { "selectWeather", "Select a weather..." },
                { "selectRecord", "Select a record..." },
                { "noQuickSelectWeathers", "No weathers added" },
                { "currentTimeUnavailable", "The current game time is unavailable." },
                { "mixedSliderValues", "Mixed values; moving the slider synchronizes this group." },
                { "settingOverrideTooltip", "Overridden by {profile}." },
                { "emptyList", "None" },
                { "weatherUnavailableLabel", "Unavailable" },
                { "weatherOverrideSuffix", " (Override)" },
                { "noPresets", "No presets installed" },
                { "selectPresetCategory", "Select a category..." },
                { "selectPreset", "Select a preset..." },
                { "settingsEditorMissingCategory", "A settings editor requires a category in 'setting'." },
                { "unsupportedSettingsCategory", "Unsupported settings category '{setting}'" },
                { "unsupportedSettingEditorPath", "Unsupported setting editor path '{setting}'" },
                { "unsupportedSliderSetting", "Unsupported slider setting '{setting}'" },
                { "unsupportedGroupedSlider", "This grouped slider contains an unsupported setting or link." },
                { "unsupportedModuleKind", "Unsupported menu module: {kind}" },
                { "profilePageUnavailable", "This Luma profile page is unavailable." },
                { "menuDefinitionUnavailable", "This profile's skseMenu.json is no longer available. Restart the game to refresh Luma's profile list." },
                { "tuningRestartRequired", "Changes to the tuning menu require restarting Skyrim." },
                { "sliderCreatorMenuLoadFailure", "The selected menu could not be loaded: {reason}" },
                { "sliderCreatorNewSlider", "New Slider" },
                { "sliderCreatorNoSettings", "No settings added" },
                { "sliderCreatorNoWeathers", "No weathers added" },
                { "noRecords", "No records added" },
                { "sliderCreatorNoContains", "No text filters added" },
                { "noPlugins", "No plugins added" },
                { "sliderCreatorUnavailableWeather", "Unavailable ({weather})" },
                { "newProfileNameHint", "Profile name" },
                { "sliderCreatorEnterSliderName", "Enter a slider name." },
                { "sliderCreatorInvalidSliderID", "The slider ID must contain only letters, numbers, underscores, or hyphens." },
                { "sliderCreatorNoValidSettings", "Add at least one valid setting to the slider." },
                { "sliderCreatorUnsupportedSettingPath", "The slider contains a setting path that TuningUtil does not support." },
                { "sliderCreatorDirectAllHues", "All Hues is a creator shortcut; direct sliders must store its seven individual hue bands." },
                { "sliderCreatorInvalidDirectLink", "A direct link requires only linkable, unfiltered settings." },
                { "sliderCreatorFilteredUnsupportedSetting", "Filtered sliders support only weather brightness, saturation, and hue-shift settings." },
                { "sliderCreatorFilteredLightingUnsupportedSetting", "Lighting Template filters support only interior brightness settings." },
                { "sliderCreatorLightingWeatherFeatures", "Time filters, local links, and saturation scales apply only to filtered weather sliders." },
                { "sliderCreatorMixedFilteredOperations", "Every setting in a filtered slider must use the same operation." },
                { "sliderCreatorInvalidLocalLink", "The local link must name a target supported by this filtered operation." },
                { "sliderCreatorHueScalesRequireSaturation", "Slider-specific saturation scales are supported only by filtered saturation sliders." },
                { "sliderCreatorLocalFeaturesRequireFilter", "Local links and slider-specific saturation scales require a filtered weather slider." },
                { "sliderCreatorInvalidHueScale", "Every slider-specific saturation scale must be a finite number." },
                { "sliderCreatorNoTimeSelected", "Select at least one time of day or disable the time filter." },
                { "sliderCreatorInvalidRange", "The slider minimum must be lower than its maximum." },
                { "sliderCreatorNegativeStep", "The slider step cannot be negative." },
                { "sliderCreatorSerializeFailure", "The updated menu JSON could not be serialized." },
                { "sliderCreatorTemporaryWriteFailure", "The temporary menu file could not be written." },
                { "sliderCreatorReplaceFailure", "The menu file could not be replaced: {reason}" },
                { "sliderCreatorPagesMissing", "The menu file does not contain a pages array." },
                { "sliderCreatorInvalidProfileName", "Enter a valid profile name that can be used as a Windows folder name." },
                { "sliderCreatorTemplateMissing", "Luma/Tuning/newSkseMenu.json is missing or does not contain a valid pages array." },
                { "sliderCreatorProfileExists", "A profile folder with that name already exists." },
                { "sliderCreatorProfileFolderFailure", "The profile folder could not be created: {reason}" },
                { "sliderCreatorProfileFolderUnknownFailure", "The profile folder could not be created." },
                { "sliderCreatorMenuWriteFailure", "The new profile's skseMenu.json could not be written." },
                { "sliderCreatorProfileSettingsWriteFailure", "The new profile's profileSettings.json could not be written." },
                { "sliderCreatorEnterPageName", "Enter a page name." },
                { "sliderCreatorPageExists", "A page with that name already exists." },
                { "sliderCreatorPageJsonFailure", "The new page JSON could not be created." },
                { "sliderCreatorMenuReadFailure", "The menu file could not be read." },
                { "sliderCreatorMenuCopyFailure", "The menu file could not be copied for editing." },
                { "sliderCreatorPageUnavailable", "The selected page is unavailable." },
                { "sliderCreatorDuplicateID", "Another slider already uses this ID." },
                { "sliderCreatorSliderJsonFailure", "The slider JSON could not be created." },
                { "sliderCreatorEditedSliderMissing", "The slider being edited no longer exists." },
                { "sliderCreatorAppendFailure", "The slider could not be added to the selected page." },
                { "layoutSelectModule", "Select a module to add." },
                { "layoutReadFailure", "The menu layout could not be read." },
                { "layoutModuleAddFailure", "The module could not be added to this page." },
                { "layoutModuleMoveBoundary", "The module cannot move farther in that direction." },
                { "layoutModuleMoveFailure", "The module order could not be changed." },
                { "layoutModuleRemoveFailure", "The module could not be removed." },
                { "layoutPageMoveBoundary", "The page cannot move farther in that direction." },
                { "layoutPageMoveFailure", "The page order could not be changed." },
                { "layoutPageRemoveFailure", "The page could not be removed. A profile must keep at least one page." },
                { "deleteUserSettingsConfirmation", "Are you sure you want to delete all user settings?" },
                { "promoteUserSettingsConfirmation", "This writes the profile's current user overrides into profileSettings.json and removes its userSettings.json. TuningUtil cannot undo this operation. Continue?" },
            };
            return messages;
        }

        const std::unordered_map<std::string, std::string>& DefaultLabels()
        {
            static const std::unordered_map<std::string, std::string> labels{
                { "savePage", "Save Page" },
                { "restorePage", "Restore Page" },
                { "resetPage", "Reset to Defaults" },
                { "saveAll", "Save All" },
                { "restoreAll", "Restore All" },
                { "resetAll", "Reset All to Defaults" },
                { "savePresetSelection", "Save Preset Selection" },
                { "presetControl", "Presets Create" },
                { "presetCreatorHeader", "Create Presets" },
                { "savePresetControl", "Save Presets" },
                { "restorePresetControl", "Restore Presets" },
                { "savePreset", "Save Preset" },
                { "removePresetOrCategory", "Remove Preset or Category" },
                { "removePreset", "Remove Preset" },
                { "removePresetCategory", "Remove Category" },
                { "addCurrentQuickSelect", "Add Current to Quick Select" },
                { "clearQuickSelect", "Clear All" },
                { "removeSelectedQuickSelect", "Remove" },
                { "applyPriority", "Apply Priority" },
                { "enableProfile", "Enable Profile" },
                { "advanced", "Advanced" },
                { "advancedModule", "Module is Advanced" },
                { "savePageEdits", "Save Page Edits" },
                { "restorePageEdits", "Restore Page Edits" },
                { "saveProfileEdits", "Save Profile Edits" },
                { "restoreProfileEdits", "Restore Profile Edits" },
                { "saveAllEdits", "Save All Edits" },
                { "restoreAllEdits", "Restore All Edits" },
                { "createPageSection", "Create Page" },
                { "createProfileSection", "Create Profile" },
                { "addSliderSetting", "Add Setting" },
                { "removeSliderSetting", "Remove" },
                { "addIncludedWeather", "Add to Include" },
                { "addExcludedWeather", "Add to Exclude" },
                { "addToIncluded", "Add to Included" },
                { "addToExcluded", "Add to Excluded" },
                { "weatherFilter", "Weather Filter" },
                { "pluginFilter", "Plugin Filter" },
                { "lightingTemplateFilter", "Lighting Template Filter" },
                { "sliderCreatorFilteredWeather", "Filtered Weather Slider" },
                { "sliderCreatorFilteredLightingTemplate", "Filtered Lighting Template Slider" },
                { "effectLightingFilter", "Effect Lighting Filter" },
                { "pointLightEffectLightingFilter", "Point Light Classification" },
                { "xemiRegion", "XEMI Region" },
                { "treatAsSunlight", "Treat as Point Light" },
                { "sunlightRegions", "Point Light Regions" },
                { "saturationScales", "Saturation Scales" },
                { "hueRanges", "Hue Ranges" },
                { "compressionAnchor", "Compression Anchor" },
                { "dynamicAmbientWithin", "Dynamic Ambient Within" },
                { "dynamicAmbientBetween", "Dynamic Ambient Between" },
                { "dynamicSunlightWithin", "Dynamic Sunlight Within" },
                { "dynamicSunlightBetween", "Dynamic Sunlight Between" },
                { "darkLimit", "Dark" },
                { "brightLimit", "Bright" },
                { "dynamicCompression", "Compression" },
                { "darkBrightness", "Dark Brightness" },
                { "brightBrightness", "Bright Brightness" },
                { "notAvailable", "N/A" },
                { "brightestWeather", "Brightest Weather" },
                { "darkestWeather", "Darkest Weather" },
                { "withinDarkestWeather", "Within Darkest Weather" },
                { "betweenDarkestWeather", "Between Darkest Weather" },
                { "interiorAmbientLinks", "Interior Ambient Links" },
                { "interiorSaturationScales", "Interior Saturation Scales" },
                { "lightingBulbSaturationScales", "Lighting Bulb Saturation Scales" },
                { "interiorLightingHueRanges", "Interior & Lighting Bulb Hue Ranges" },
                { "weatherFilterWeather", "Weather" },
                { "lightingTemplate", "Lighting Template" },
                { "fxWeather", "FX Weather" },
                { "includedWeathers", "Included Weathers" },
                { "excludedWeathers", "Excluded Weathers" },
                { "includedRecords", "Included Records" },
                { "excludedRecords", "Excluded Records" },
                { "includedList", "Included List" },
                { "excludedList", "Excluded List" },
                { "includedPlugins", "Included Plugins" },
                { "excludedPlugins", "Excluded Plugins" },
                { "pluginName", "Plugin" },
                { "pluginNameHint", "Full plugin filename, such as Skyrim.esm" },
                { "addOrUpdatePlugin", "Add / Update Plugin" },
                { "clearPlugins", "Clear Plugins" },
                { "pluginList", "Plugin List" },
                { "removePlugin", "Remove Plugin" },
                { "removeFilteredWeather", "Remove Weather" },
                { "removeRecord", "Remove Record" },
                { "contains", "Contains" },
                { "containsHint", "EditorID contains text" },
                { "pluginContainsHint", "Plugin filename contains text" },
                { "addOrUpdateContains", "Add / Update Contains" },
                { "clearContains", "Clear Contains" },
                { "containsList", "Contains List" },
                { "removeContains", "Remove Contains" },
                { "addSlider", "Add Slider" },
                { "updateSlider", "Update Slider" },
                { "createProfile", "Create New Profile" },
                { "copyExistingProfile", "Copy Existing Profile" },
                { "blankProfile", "Blank Profile" },
                { "deleteUserSettings", "Delete User Settings" },
                { "confirmDeleteUserSettings", "Delete" },
                { "cancelDeleteUserSettings", "Cancel" },
                { "forceCSTonemapping", "Force CS Tonemapping" },
                { "makeUserSettingsPermanent", "Make User Settings Permanent" },
                { "makeUserSettingsPermanentTitle", "Make User Settings Permanent?" },
                { "confirmMakeUserSettingsPermanent", "Make Permanent" },
                { "cancelMakeUserSettingsPermanent", "Cancel" },
                { "displayIniSection", "[Display]" },
                { "unavailableValue", "Unavailable" },
                { "sliderCreatorCategory", "Category" },
                { "sliderCreatorSetting", "Setting" },
                { "sliderCreatorValue", "Value" },
                { "sliderCreatorLighting", "Lighting" },
                { "sliderCreatorWeather", "Weather" },
                { "sliderCreatorSliderSection", "Slider" },
                { "sliderCreatorSettingsSection", "Settings" },
                { "sliderCreatorAdvancedSettings", "Advanced Settings" },
                { "sliderCreatorAutomatic", "Automatic" },
                { "sliderCreatorFormat", "Format" },
                { "sliderCreatorScale", "Scale" },
                { "sliderCreatorIgnoreLink", "Ignore Link" },
                { "sliderCreatorLink", "Link" },
                { "sliderCreatorLinkHint", "Optional direct grouped link source" },
                { "sliderCreatorInvert", "Invert Slider" },
                { "sliderCreatorCustomDefault", "Custom Default" },
                { "sliderCreatorDefault", "Default" },
                { "sliderCreatorCustomRange", "Custom Range" },
                { "sliderCreatorMinimum", "Minimum" },
                { "sliderCreatorMaximum", "Maximum" },
                { "sliderCreatorStep", "Step" },
                { "sliderCreatorCustomWidth", "Custom Width" },
                { "sliderCreatorWidth", "Width" },
                { "sliderCreatorFunctionalPreview", "Functional Preview" },
                { "sliderCreatorAddSliderSection", "Add Slider" },
                { "sliderCreatorLoadExisting", "Load Existing Slider" },
                { "sliderCreatorPageSelection", "Page Selection" },
                { "addAllHues", "Add All Hues" },
                { "sliderCreatorLocalLink", "Local Link" },
                { "sliderCreatorNoLocalLink", "None" },
                { "sliderCreatorUniqueHueScales", "Unique Saturation Scales" },
                { "editMode", "Edit Profile Mode" },
                { "editPage", "Edit Page" },
                { "newPage", "New Page" },
                { "addPage", "Add Page" },
                { "pageOrder", "Page Order" },
                { "renamePageSection", "Rename Page" },
                { "pageName", "Page Name" },
                { "renamePage", "Rename Page" },
                { "movePageUp", "Move Page Up" },
                { "movePageDown", "Move Page Down" },
                { "deletePage", "Delete Page" },
                { "addModule", "Add Module" },
                { "module", "Module" },
                { "addSelectedModule", "Add Selected Module" },
                { "addElement", "Add Element" },
                { "element", "Element" },
                { "elementText", "Text" },
                { "addSelectedElement", "Add Selected Element" },
                { "pageContents", "Page Contents" },
                { "moveUp", "Up" },
                { "moveDown", "Down" },
                { "removeModule", "Remove" },
            };
            return labels;
        }

        std::string Lowercase(std::string a_value)
        {
            std::ranges::transform(a_value, a_value.begin(), [](const unsigned char a_character)
                { return static_cast<char>(std::tolower(a_character)); });
            return a_value;
        }

        void NormalizeOptionalJsonColor(std::optional<Color>& a_color)
        {
            if (!a_color)
            {
                return;
            }
            NormalizeJsonColor(*a_color);
        }

        std::string SliderKind(const std::string_view a_setting)
        {
            const auto setting = Lowercase(std::string(a_setting));
            if (setting.contains("compressionanchor") || setting.contains("anchor")) return "anchor";
            if (setting.contains("compression") || setting.contains("between") || setting.contains("within")) return "compression";
            if (setting.contains("huerange")) return "huerange";
            if (setting.contains("huescale")) return "huescale";
            if (setting.contains("hueshift")) return "hueshift";
            if (setting.contains("linkscale")) return "linkscale";
            if (setting.contains("timeofday")) return "timeofday";
            if (setting.contains("saturation")) return "saturation";
            if (setting.contains("brightness")) return "brightness";
            if (setting.contains("multiplier") || setting.contains("imagespace") ||
                setting.contains("fogmax") || setting.contains("volumetric") ||
                setting.contains("pointlight"))
            {
                return "multiplier";
            }
            return "generic";
        }

        std::optional<std::string> ReadText(const bool a_stripUtf8Bom = true)
        {
            std::ifstream file(kSettingsPath, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            std::string text(std::istreambuf_iterator<char>(file), {});
            constexpr std::string_view utf8Bom = "\xEF\xBB\xBF";
            if (a_stripUtf8Bom && text.starts_with(utf8Bom)) text.erase(0, utf8Bom.size());
            return text;
        }

        bool WriteTuningMenuEnabled(const bool a_enabled)
        {
            auto text = ReadText(false);
            if (!text)
            {
                logger::warn("[Tuning Menu] settings read failed | path={}", kSettingsPath.string());
                return false;
            }

            static const std::regex pattern(
                R"(("enableTuningMenu"\s*:\s*)(true|false))",
                std::regex::icase);
            std::smatch match;
            if (std::regex_search(*text, match, pattern))
            {
                text->replace(
                    static_cast<std::size_t>(match.position(2)),
                    static_cast<std::size_t>(match.length(2)),
                    a_enabled ? "true" : "false");
            }
            else
            {
                const auto object = text->find('{');
                if (object == std::string::npos)
                {
                    logger::warn(
                        "[Tuning Menu] setting save failed | {} is not a JSON object",
                        kSettingsPath.string());
                    return false;
                }
                const auto content = text->find_first_not_of(
                    " \t\r\n",
                    object + 1);
                const bool empty =
                    content != std::string::npos && (*text)[content] == '}';
                std::string setting = "\n    \"enableTuningMenu\": ";
                setting += a_enabled ? "true" : "false";
                setting += empty ? "\n" : ",";
                text->insert(object + 1, setting);
            }

            std::ofstream file(
                kSettingsPath,
                std::ios::binary | std::ios::trunc);
            file << *text;
            if (!file)
            {
                logger::warn(
                    "[Tuning Menu] setting save failed | path={}",
                    kSettingsPath.string());
                return false;
            }
            return true;
        }

        void Load(const bool a_initialLoad)
        {
            std::error_code error;
            const auto exists = std::filesystem::is_regular_file(kSettingsPath, error) && !error;
            const auto writeTime = exists ?
                                       std::optional{ std::filesystem::last_write_time(kSettingsPath, error) } :
                                       std::nullopt;
            if (!a_initialLoad && writeTime == loadedWriteTime)
            {
                return;
            }

            Settings loaded;
            if (const auto text = exists ? ReadText() : std::nullopt)
            {
                const auto parsed = rfl::json::read<Settings, rfl::DefaultIfMissing>(*text);
                if (!parsed)
                {
                    logger::warn("[Tuning Menu] settings load failed | path={} | {}", kSettingsPath.string(), parsed.error().what());
                    loadedWriteTime = writeTime;
                    return;
                }
                loaded = parsed.value();
            }
            else if (exists)
            {
                logger::warn("[Tuning Menu] settings read failed | path={}", kSettingsPath.string());
                loadedWriteTime = writeTime;
                return;
            }

            const auto location = Lowercase(loaded.statusDisplay.location);
            if (location != "top" && location != "bottom" && location != "hidden")
            {
                logger::warn("[Tuning Menu] status location={} invalid | fallback=bottom", loaded.statusDisplay.location);
                loaded.statusDisplay.location = "bottom";
            }
            else
            {
                loaded.statusDisplay.location = location;
            }
            if (!std::isfinite(loaded.statusDisplay.duration) || loaded.statusDisplay.duration < 0.0)
            {
                loaded.statusDisplay.duration = 0.0;
            }
            if (!std::isfinite(loaded.statusDisplay.fontScale) || loaded.statusDisplay.fontScale <= 0.0f)
            {
                loaded.statusDisplay.fontScale = 1.0f;
            }
            if (!std::isfinite(loaded.statusDisplay.height) || loaded.statusDisplay.height <= 0.0f)
            {
                loaded.statusDisplay.height = 48.0f;
            }
            NormalizeOptionalJsonColor(loaded.statusDisplay.color);
            loaded.buttonFeedback.hoverBrightness = std::clamp(loaded.buttonFeedback.hoverBrightness, 0.0f, 1.0f);
            loaded.buttonFeedback.pressedBrightness = std::clamp(loaded.buttonFeedback.pressedBrightness, 0.0f, 1.0f);

            NormalizeOptionalJsonColor(loaded.buttonColors.ordinary);
            NormalizeOptionalJsonColor(loaded.buttonColors.save);
            NormalizeOptionalJsonColor(loaded.buttonColors.restore);
            NormalizeOptionalJsonColor(loaded.buttonColors.reset);
            NormalizeOptionalJsonColor(loaded.buttonColors.destructive);
            NormalizeJsonColor(loaded.presetColors.active);
            NormalizeJsonColor(loaded.presetColors.activeHovered);
            NormalizeJsonColor(loaded.presetColors.activePressed);
            NormalizeJsonColor(loaded.presetColors.modified);
            NormalizeJsonColor(loaded.presetColors.modifiedHovered);
            NormalizeJsonColor(loaded.presetColors.modifiedPressed);

            const auto finiteSpacing = [](float& a_value, const float a_fallback)
            {
                if (!std::isfinite(a_value)) a_value = a_fallback;
            };
            finiteSpacing(loaded.layout.actionButtonSpacing, -1.0f);
            finiteSpacing(loaded.layout.pageTopSpacing, 0.0f);
            finiteSpacing(loaded.layout.sectionSpacing, 0.0f);
            if (!std::isfinite(loaded.layout.headerFontScale) || loaded.layout.headerFontScale <= 0.0f)
                loaded.layout.headerFontScale = 1.25f;
            for (auto& value : loaded.layout.boxPadding) finiteSpacing(value, 0.0f);

            std::unordered_map<std::string, ConfiguredSliderDefaults> normalizedSliderDefaults;
            for (auto& [key, value] : loaded.sliderDefaults)
            {
                if (std::isfinite(value.step) && value.step < 0.0f) value.step = 0.0f;
                normalizedSliderDefaults.insert_or_assign(Lowercase(key), std::move(value));
            }
            loaded.sliderDefaults = std::move(normalizedSliderDefaults);

            const auto normalizeStringMap = [](std::unordered_map<std::string, std::string>& a_values)
            {
                std::unordered_map<std::string, std::string> normalized;
                for (auto& [key, value] : a_values)
                    normalized.insert_or_assign(Lowercase(key), std::move(value));
                a_values = std::move(normalized);
            };
            normalizeStringMap(loaded.settingLabels.groups);
            normalizeStringMap(loaded.settingLabels.names);
            normalizeStringMap(loaded.settingLabels.hues);
            normalizeStringMap(loaded.settingLabels.settings);

            loaded.settingsProfile.position = Lowercase(loaded.settingsProfile.position);
            if (loaded.settingsProfile.position != "top" && loaded.settingsProfile.position != "bottom")
            {
                logger::warn(
                    "[Tuning Menu] Settings profile position={} invalid | fallback=bottom",
                    loaded.settingsProfile.position);
                loaded.settingsProfile.position = "bottom";
            }
            if (loaded.settingsProfile.label.empty()) loaded.settingsProfile.label = "Settings";
            loaded.settingsProfile.blankLinesAbove = std::clamp(loaded.settingsProfile.blankLinesAbove, 0, 4);

            loaded.tooltip.delay = Lowercase(loaded.tooltip.delay);
            if (loaded.tooltip.delay != "none" && loaded.tooltip.delay != "short" && loaded.tooltip.delay != "normal")
            {
                loaded.tooltip.delay = "normal";
            }
            if (!std::isfinite(loaded.tooltip.fontScale) || loaded.tooltip.fontScale <= 0.0f)
            {
                loaded.tooltip.fontScale = 1.0f;
            }
            NormalizeOptionalJsonColor(loaded.tooltip.textColor);
            NormalizeOptionalJsonColor(loaded.tooltip.backgroundColor);

            settings = std::move(loaded);
            loadedWriteTime = writeTime;
            logger::info(
                "[Tuning Menu] presentation | source={} | file={}",
                kSettingsPath.string(),
                exists);
        }

        std::string ResolveMessage(
            const std::unordered_map<std::string, std::string>& a_configured,
            const std::unordered_map<std::string, std::string>& a_defaults,
            const std::string_view a_key,
            const std::initializer_list<MessageArgument> a_arguments)
        {
            const auto key = std::string(a_key);
            const auto configured = a_configured.find(key);
            const auto fallback = a_defaults.find(key);
            auto result = configured != a_configured.end() ?
                              configured->second :
                              fallback != a_defaults.end() ? fallback->second : key;
            for (const auto& [name, value] : a_arguments)
            {
                const auto token = "{" + std::string(name) + "}";
                for (auto position = result.find(token); position != std::string::npos; position = result.find(token, position + value.size()))
                {
                    result.replace(position, token.size(), value);
                }
            }
            return result;
        }
    }  // namespace

    void NormalizeJsonColor(Color& a_color)
    {
        for (std::size_t index = 0; index < 3; ++index)
        {
            auto& channel = a_color[index];
            channel = std::isfinite(channel) ? std::clamp(channel, 0.0f, 255.0f) / 255.0f : 1.0f;
        }
        auto& alpha = a_color[3];
        alpha = std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
    }

    void Initialize()
    {
        if (!initialized)
        {
            initialized = true;
            Load(true);
        }
    }

    void ReloadIfChanged()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextCheck)
        {
            return;
        }
        nextCheck = now + std::chrono::seconds(1);
        Load(false);
    }

    bool GetTuningMenuEnabled()
    {
        Initialize();
        return settings.enableTuningMenu;
    }

    bool SetTuningMenuEnabled(const bool a_enabled)
    {
        Initialize();
        if (!WriteTuningMenuEnabled(a_enabled))
        {
            return false;
        }
        settings.enableTuningMenu = a_enabled;
        Load(true);
        return true;
    }

    StatusLocation GetStatusLocation()
    {
        if (settings.statusDisplay.location == "top") return StatusLocation::top;
        if (settings.statusDisplay.location == "hidden") return StatusLocation::hidden;
        return StatusLocation::bottom;
    }

    std::chrono::duration<double> GetStatusDuration()
    {
        return std::chrono::duration<double>(settings.statusDisplay.duration);
    }

    float GetStatusFontScale()
    {
        return settings.statusDisplay.fontScale;
    }

    float GetStatusHeight()
    {
        return settings.statusDisplay.height;
    }

    std::optional<Color> GetStatusColor()
    {
        return settings.statusDisplay.color;
    }

    bool ClearStatusOnPageChange()
    {
        return settings.statusDisplay.clearOnPageChange;
    }

    float GetHoverBrightness()
    {
        return settings.buttonFeedback.hoverBrightness;
    }

    float GetPressedBrightness()
    {
        return settings.buttonFeedback.pressedBrightness;
    }

    std::optional<Color> GetButtonColor(const ButtonKind a_kind)
    {
        switch (a_kind)
        {
        case ButtonKind::save:
            return settings.buttonColors.save;
        case ButtonKind::restore:
            return settings.buttonColors.restore;
        case ButtonKind::reset:
            return settings.buttonColors.reset;
        case ButtonKind::destructive:
            return settings.buttonColors.destructive;
        default:
            return settings.buttonColors.ordinary;
        }
    }

    Color GetPresetColor(const PresetState a_state, const InteractionState a_interaction)
    {
        const auto modified = a_state == PresetState::modified;
        if (a_interaction == InteractionState::hovered)
        {
            return modified ? settings.presetColors.modifiedHovered : settings.presetColors.activeHovered;
        }
        if (a_interaction == InteractionState::pressed)
        {
            return modified ? settings.presetColors.modifiedPressed : settings.presetColors.activePressed;
        }
        return modified ? settings.presetColors.modified : settings.presetColors.active;
    }

    std::string Label(const std::string_view a_key, const std::string_view a_fallback)
    {
        const auto key = std::string(a_key);
        if (const auto configured = settings.labels.find(key); configured != settings.labels.end())
        {
            return configured->second;
        }
        if (const auto fallback = DefaultLabels().find(key); fallback != DefaultLabels().end())
        {
            return fallback->second;
        }
        return std::string(a_fallback);
    }

    namespace
    {
        std::string ConfiguredSettingLabel(
            const std::unordered_map<std::string, std::string>& a_values,
            const std::string_view a_key,
            const std::string_view a_fallback)
        {
            const auto configured = a_values.find(Lowercase(std::string(a_key)));
            return configured != a_values.end() ? configured->second : std::string(a_fallback);
        }
    }

    std::string SettingGroupLabel(const std::string_view a_key, const std::string_view a_fallback)
    {
        return ConfiguredSettingLabel(settings.settingLabels.groups, a_key, a_fallback);
    }

    std::string SettingNameLabel(const std::string_view a_key, const std::string_view a_fallback)
    {
        return ConfiguredSettingLabel(settings.settingLabels.names, a_key, a_fallback);
    }

    std::string SettingHueLabel(const std::string_view a_key, const std::string_view a_fallback)
    {
        return ConfiguredSettingLabel(settings.settingLabels.hues, a_key, a_fallback);
    }

    std::string SettingPathLabel(const std::string_view a_path, const std::string_view a_fallback)
    {
        return ConfiguredSettingLabel(settings.settingLabels.settings, a_path, a_fallback);
    }

    std::string SettingLabelSeparator()
    {
        return settings.settingLabels.separator;
    }

    float GetActionButtonSpacing()
    {
        return settings.layout.actionButtonSpacing;
    }

    float GetPageTopSpacing()
    {
        return settings.layout.pageTopSpacing;
    }

    float GetSectionSpacing()
    {
        return settings.layout.sectionSpacing;
    }

    float GetHeaderFontScale()
    {
        return settings.layout.headerFontScale;
    }

    std::array<float, 2> GetBoxPadding()
    {
        return settings.layout.boxPadding;
    }

    SliderDefaults ResolveSliderDefaults(
        const std::string_view a_setting,
        const float a_fallbackWidth,
        const float a_fallbackStep,
        const std::string_view a_fallbackFormat,
        const float a_fallbackMinimum,
        const float a_fallbackMaximum)
    {
        const auto exactKey = Lowercase(std::string(a_setting));
        const auto kind = SliderKind(a_setting);
        SliderDefaults result{
            a_fallbackMinimum,
            a_fallbackMaximum,
            a_fallbackWidth,
            a_fallbackStep,
            std::string(a_fallbackFormat),
        };
        const auto apply = [&](const std::string& a_key)
        {
            const auto configured = settings.sliderDefaults.find(a_key);
            if (configured == settings.sliderDefaults.end()) return;
            const auto& source = configured->second;
            if (std::isfinite(source.min)) result.minimum = source.min;
            if (std::isfinite(source.max)) result.maximum = source.max;
            if (std::isfinite(source.width)) result.width = source.width;
            if (std::isfinite(source.step)) result.step = source.step;
            if (!source.format.empty()) result.format = source.format;
        };
        apply("generic");
        apply(kind);
        apply(exactKey);
        return result;
    }

    bool SettingsProfileFirst()
    {
        return settings.settingsProfile.position == "top";
    }

    std::string SettingsProfileLabel(const bool a_includeGap)
    {
        auto label = settings.settingsProfile.label;
        if (a_includeGap && settings.settingsProfile.position == "bottom")
        {
            label.insert(0, static_cast<std::size_t>(settings.settingsProfile.blankLinesAbove), '\n');
        }
        return label;
    }

    TooltipDelay GetTooltipDelay()
    {
        if (settings.tooltip.delay == "none") return TooltipDelay::none;
        if (settings.tooltip.delay == "short") return TooltipDelay::shortDelay;
        return TooltipDelay::normal;
    }

    float GetTooltipFontScale()
    {
        return settings.tooltip.fontScale;
    }

    std::optional<Color> GetTooltipTextColor()
    {
        return settings.tooltip.textColor;
    }

    std::optional<Color> GetTooltipBackgroundColor()
    {
        return settings.tooltip.backgroundColor;
    }

    std::string StatusMessage(
        const std::string_view a_key,
        const std::initializer_list<MessageArgument> a_arguments)
    {
        return ResolveMessage(settings.statusMessages, DefaultStatusMessages(), a_key, a_arguments);
    }

    std::string DisplayMessage(
        const std::string_view a_key,
        const std::initializer_list<MessageArgument> a_arguments)
    {
        return ResolveMessage(settings.displayMessages, DefaultDisplayMessages(), a_key, a_arguments);
    }
}  // namespace MPL::SKSEMenuSettings
