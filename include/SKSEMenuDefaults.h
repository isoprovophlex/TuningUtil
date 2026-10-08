#pragma once

#include <string_view>

namespace MPL::SKSEMenuSettings
{
    inline constexpr std::string_view BuiltInDefaults = R"LumaDefaults(
{
    "statusDisplay": {
        "location": "top",
        "duration": 5,
        "fontScale": 1,
        "height": 36,
        "color": [
            255,
            255,
            255,
            1
        ],
        "clearOnPageChange": false
    },
    "buttonFeedback": {
        "hoverBrightness": 0.12,
        "pressedBrightness": 0.35
    },
    "buttonColors": {
        "ordinary": [
            77,
            77,
            77,
            1
        ],
        "save": [
            43,
            93,
            157,
            1
        ],
        "restore": [
            102,
            102,
            102,
            1
        ],
        "reset": [
            120,
            40,
            40,
            1
        ],
        "destructive": [
            120,
            40,
            40,
            1
        ]
    },
    "presetColors": {
        "active": [
            43,
            93,
            157,
            1
        ],
        "activeHovered": [
            68,
            112,
            169,
            1
        ],
        "activePressed": [
            117,
            150,
            191,
            1
        ],
        "modified": [
            120,
            40,
            40,
            1
        ],
        "modifiedHovered": [
            153,
            66,
            66,
            1
        ],
        "modifiedPressed": [
            176,
            115,
            115,
            1
        ]
    },
    "labels": {
        "savePage": "Save Page",
        "restorePage": "Restore Page",
        "resetPage": "Reset to Defaults",
        "saveAll": "Save All Pages",
        "restoreAll": "Restore All Pages",
        "resetAll": "Reset All Pages to Default",
        "savePresetSelection": "Save Preset Settings",
        "clearQuickSelect": "Clear All",
        "enableProfile": "Enable Profile",
        "advanced": "Show Advanced Pages",
        "addSliderSetting": "Add Setting",
        "removeSliderSetting": "Remove",
        "addIncludedWeather": "Add to Include",
        "addExcludedWeather": "Add to Exclude",
        "removeFilteredWeather": "Remove Weather",
        "addOrUpdateContains": "Add / Update Contains",
        "clearContains": "Clear Contains",
        "removeContains": "Remove Contains",
        "updateSlider": "Update Slider",
        "createProfile": "Create New Profile",
        "sliderCreatorCategory": "Category",
        "sliderCreatorSetting": "Setting",
        "addAllHues": "Add All Hues",
        "sliderCreatorUniqueHueScales": "Unique Saturation Scales"
    },
    "settingLabels": {
        "separator": " / ",
        "groups": {
            "Brightness": "Brightness",
            "Saturation": "Saturation",
            "Hue Shift": "Hue Shift",
            "Between Weather Compression": "Between Weather Compression",
            "Within Weather Compression": "Within Weather Compression",
            "Compression Anchors": "Compression Anchors",
            "Saturation Scales": "Saturation Scales",
            "Hue Ranges": "Hue Ranges",
            "Volumetric Lighting": "Volumetric Lighting",
            "Image Space": "Image Space",
            "Fog": "Fog",
            "Effect Lighting": "Effect Lighting",
            "Point Lights": "Point Lights"
        },
        "names": {
            "Ambient": "Ambient",
            "Sunlight": "Sunlight",
            "Effect Lighting": "Effect Lighting",
            "Fog Far": "Fog Far",
            "Fog Near": "Fog Near",
            "Water": "Water",
            "Sky Statics": "Sky Statics",
            "Sky Upper": "Sky Upper",
            "Sky Lower": "Sky Lower",
            "Horizon": "Horizon",
            "Sun": "Sun",
            "Sun Glare": "Sun Glare",
            "Moon Glare": "Moon Glare",
            "Stars": "Stars",
            "Cloud Layers": "Cloud Layers",
            "Volumetric Lighting": "Volumetric Lighting",
            "Ambient Colors (DALC)": "Ambient Colors (DALC)",
            "Directional": "Directional",
            "Saturation": "Saturation",
            "Brightness": "Brightness",
            "Contrast": "Contrast",
            "Sunlight Scale": "Sunlight Scale",
            "Sky Scale": "Sky Scale",
            "Intensity": "Intensity",
            "Fog Strength": "Fog Strength",
            "Fade": "Fade",
            "Hue Shift": "Hue Shift",
            "Saturation Scale": "Saturation Scale",
            "All Hues": "All Hues",
            "Start": "Start",
            "End": "End"
        },
        "hues": {
            "red": "Red",
            "orange": "Orange",
            "yellow": "Yellow",
            "green": "Green",
            "teal": "Teal",
            "blue": "Blue",
            "magenta": "Magenta"
        },
        "settings": {}
    },
    "layout": {
        "actionButtonSpacing": 12,
        "pageTopSpacing": 0,
        "sectionSpacing": 0,
        "boxPadding": [
            8,
            8
        ]
    },
    "sliderDefaults": {
        "generic": {
            "min": 0,
            "max": 2,
            "width": 0,
            "step": 0,
            "format": "%.2f"
        },
        "multiplier": {
            "min": 0,
            "max": 4,
            "width": 0,
            "step": 0.1,
            "format": "%.1f"
        },
        "brightness": {
            "min": 0.1,
            "max": 4,
            "width": 0,
            "step": 0.1,
            "format": "%.1f"
        },
        "saturation": {
            "min": 0,
            "max": 6,
            "width": 0,
            "step": 0.1,
            "format": "%.1f"
        },
        "compression": {
            "min": -200,
            "max": 200,
            "width": 0,
            "step": 10,
            "format": "%.0f"
        },
        "anchor": {
            "min": 0,
            "max": 255,
            "width": 0,
            "step": 1,
            "format": "%.0f"
        },
        "hueScale": {
            "min": 0,
            "max": 4,
            "width": 0,
            "step": 0.1,
            "format": "%.1f"
        },
        "hueShift": {
            "min": -180,
            "max": 180,
            "width": 0,
            "step": 0.1,
            "format": "%.1f"
        },
        "hueRange": {
            "min": 0,
            "max": 255,
            "width": 0,
            "step": 0.1,
            "format": "%.0f"
        },
        "linkScale": {
            "min": 0,
            "max": 4,
            "width": 0,
            "step": 0.1,
            "format": "%.1f"
        },
        "timeOfDay": {
            "min": 0,
            "max": 24,
            "width": 0,
            "step": 0,
            "format": "%.2f"
        }
    },
    "settingsProfile": {
        "position": "bottom",
        "label": "Settings",
        "blankLinesAbove": 1
    },
    "statusMessages": {
        "profileMenusReloaded": "Reloaded {count} profile menu(s).",
        "quickSelectSaveFailure": "Quick Select changed for this session, but its list could not be saved.",
        "quickSelectClearSaveFailure": "Quick Select was cleared for this session, but its list could not be saved.",
        "weatherLockUnavailable": "Weather lock is enabled by this profile, but no current weather is available.",
        "profileStateSaveFailure": "The profile state could not be saved, so it was not changed.",
        "profileEnabled": "Profile enabled and saved.",
        "profileDisabled": "Profile disabled and saved.",
        "presetPreview": "Previewing {category} preset {preset}.",
        "presetLoadFailure": "The preset could not be loaded. Check the LumaUtil log for details.",
        "saveAllSuccess": "Saved all {profile} page settings.",
        "saveAllFailure": "Settings could not be saved. Check the LumaUtil log for details.",
        "pageHasNoSettings": "This page has no settings that can be reset.",
        "resetPageSuccess": "Default settings loaded for this page. Select Save Page to keep these changes.",
        "resetPageFailure": "Page settings could not be reset. Check the LumaUtil log for details.",
        "savePageSuccess": "Saved settings for this page.",
        "savePageFailure": "Page settings could not be saved. Check the LumaUtil log for details.",
        "restorePageSuccess": "Saved settings restored for this page.",
        "restorePageFailure": "Page settings could not be restored. Check the LumaUtil log for details.",
        "restoreAllSuccess": "Saved page settings restored for this profile.",
        "restoreAllFailure": "Profile settings could not be restored. Check the LumaUtil log for details.",
        "resetAllSuccess": "Default page settings loaded for this profile. Select Save All Pages to keep these changes.",
        "resetAllFailure": "Profile settings could not be reset. Check the LumaUtil log for details.",
        "prioritySaved": "Profile priority applied and saved. The Luma menu order will use it on the next game launch.",
        "prioritySaveFailure": "Profile priority applied for this session, but it could not be saved.",
        "tuningMenuEnabled": "Tuning menus will be enabled after restarting Skyrim.",
        "tuningMenuDisabled": "Tuning menus will be disabled after restarting Skyrim. Profiles will continue to apply at startup.",
        "tuningMenuSaveFailure": "The tuning-menu setting could not be saved.",
        "detailedLoggingEnabled": "Detailed logging enabled.",
        "detailedLoggingDisabled": "Detailed logging disabled.",
        "detailedLoggingSaveFailure": "The detailed-logging setting could not be saved.",
        "sliderSaved": "Saved {slider} to {profile} / {page}.",
        "sliderSaveFailure": "The slider could not be saved: {reason}",
        "profileCreated": "Created profile {profile}. Restart Skyrim to load its menu.",
        "profileCreateFailure": "The profile could not be created: {reason}"
    },
    "displayMessages": {
        "noSelectableWeathers": "No selectable weathers were found for this profile's loaded plugins.",
        "selectWeather": "Select a weather...",
        "noQuickSelectWeathers": "No weathers added",
        "currentTimeUnavailable": "The current game time is unavailable.",
        "mixedSliderValues": "Mixed values; moving the slider synchronizes this group.",
        "emptyList": "None",
        "weatherUnavailableLabel": "Unavailable",
        "noPresets": "No presets installed",
        "settingsEditorMissingCategory": "A settings editor requires a category in 'setting'.",
        "unsupportedSettingsCategory": "Unsupported settings category '{setting}'",
        "unsupportedSliderSetting": "Unsupported slider setting '{setting}'",
        "profilePageUnavailable": "This Luma profile page is unavailable.",
        "menuDefinitionUnavailable": "This profile's skseMenu.json is no longer available. Restart the game to refresh Luma's profile list.",
        "sliderCreatorMenuLoadFailure": "The selected menu could not be loaded: {reason}",
        "sliderCreatorNewSlider": "New Slider",
        "sliderCreatorNoSettings": "No settings added",
        "sliderCreatorNoWeathers": "No weathers added",
        "sliderCreatorNoContains": "No text filters added",
        "sliderCreatorEnterSliderName": "Enter a slider name.",
        "sliderCreatorInvalidSliderID": "The slider ID must contain only letters, numbers, underscores, or hyphens.",
        "sliderCreatorNoValidSettings": "Add at least one valid setting to the slider.",
        "sliderCreatorUnsupportedSettingPath": "The slider contains a setting path that TuningUtil does not support.",
        "sliderCreatorFilteredUnsupportedSetting": "Filtered sliders support only weather brightness, saturation, and hue-shift settings.",
        "sliderCreatorMixedFilteredOperations": "Every setting in a filtered slider must use the same operation.",
        "sliderCreatorHueScalesRequireSaturation": "Slider-specific saturation scales are supported only by filtered saturation sliders.",
        "sliderCreatorInvalidHueScale": "Every slider-specific saturation scale must be a finite number.",
        "sliderCreatorNoTimeSelected": "Select at least one time of day or disable the time filter.",
        "sliderCreatorInvalidRange": "The slider minimum must be lower than its maximum.",
        "sliderCreatorNegativeStep": "The slider step cannot be negative.",
        "sliderCreatorSerializeFailure": "The updated menu JSON could not be serialized.",
        "sliderCreatorTemporaryWriteFailure": "The temporary menu file could not be written.",
        "sliderCreatorReplaceFailure": "The menu file could not be replaced: {reason}",
        "sliderCreatorPagesMissing": "The menu file does not contain a pages array.",
        "sliderCreatorInvalidProfileName": "Enter a valid profile name that can be used as a Windows folder name.",
        "sliderCreatorProfileExists": "A profile folder with that name already exists.",
        "sliderCreatorProfileFolderFailure": "The profile folder could not be created: {reason}",
        "sliderCreatorProfileFolderUnknownFailure": "The profile folder could not be created.",
        "sliderCreatorMenuWriteFailure": "The new profile's skseMenu.json could not be written.",
        "sliderCreatorProfileSettingsWriteFailure": "The new profile's profileSettings.json could not be written.",
        "sliderCreatorEnterPageName": "Enter a page name.",
        "sliderCreatorPageExists": "A page with that name already exists.",
        "sliderCreatorPageJsonFailure": "The new page JSON could not be created.",
        "sliderCreatorMenuReadFailure": "The menu file could not be read.",
        "sliderCreatorMenuCopyFailure": "The menu file could not be copied for editing.",
        "sliderCreatorPageUnavailable": "The selected page is unavailable.",
        "sliderCreatorDuplicateID": "Another slider already uses this ID.",
        "sliderCreatorSliderJsonFailure": "The slider JSON could not be created.",
        "sliderCreatorEditedSliderMissing": "The slider being edited no longer exists.",
        "sliderCreatorAppendFailure": "The slider could not be added to the selected page."
    }
}
)LumaDefaults";
}
