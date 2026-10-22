// Copyright 2023, Shawn Wallace
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenVR IVRSettings interface implementation.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#include <openvr_driver.h>
#include <algorithm>
#include <optional>
#include <cstring>
#include <map>
#include <string_view>

#include "settings.hpp"
#include "context.hpp"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_json.h"
#include "util/u_config_json.h"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace std::string_view_literals;

// Default to 100% brightness.
DEBUG_GET_ONCE_FLOAT_OPTION(lh_default_brightness, "LH_DEFAULT_BRIGHTNESS", 1.0)
DEBUG_GET_ONCE_BOOL_OPTION(lh_standby_on_exit, "LH_STANDBY_ON_EXIT", false)

namespace {
cJSON *
load_json(const std::string &path)
{
	std::ifstream file(path);
	if (!file) {
		return nullptr;
	}
	std::stringstream contents;
	contents << file.rdbuf();
	return cJSON_Parse(contents.str().c_str());
}

std::string
user_settings_path(const std::string &steam_install)
{
	if (const char *path = getenv("STEAMVR_LH_SETTINGS")) {
		return path;
	}
	// SteamOS (e.g. the Steam Frame) keeps the runtime config outside the Steam install
	std::string openvr = std::string(getenv("HOME")) + "/.config/openvr/config/steamvr.vrsettings";
	if (std::filesystem::exists(openvr)) {
		return openvr;
	}
	return steam_install + "/config/steamvr.vrsettings";
}
} // namespace

Settings::Settings(const std::string &steam_install, const std::string &steamvr_install, Context *context)
    : overrides(cJSON_CreateObject()), context{context}
{
	const char *driver = getenv("STEAMVR_LH_DRIVER");
	std::string driver_name = driver ? driver : "lighthouse";
	for (const std::string &path : {user_settings_path(steam_install),
	                                steamvr_install + "/drivers/" + driver_name + "/resources/settings/default.vrsettings",
	                                steamvr_install + "/resources/settings/default.vrsettings"}) {
		if (cJSON *layer = load_json(path)) {
			layers.push_back(layer);
		} else {
			U_LOG_W("Couldn't load settings from %s", path.c_str());
		}
	}
	// Values SteamVR provides at runtime rather than through settings files (as its logs show them)
	if (driver_name == "cv") {
		cJSON *layer = cJSON_Parse(R"({
		    "steamvr": {"allowEyeTracking": true, "eyeTrackingRateMax": 90.0},
		    "driver_cv": {"controllerTracking3": "alternate", "infraredEmittersMode": 2,
		                  "enableHmdAngularAcceleration": true}
		})");
		// The eye cameras' rate: driver_cv asks the eye service for eyeTrackingRateMin while gaze dependent
		// eye-to-head is off (frame_hmd's settings: 15 fps); frame.eye_tracking_fps in Monado's config
		float eye_fps = 72.0f;
		struct u_config_json json = {};
		u_config_json_open_or_create_main_file(&json);
		const cJSON *frame = json.root != nullptr ? u_json_get(json.root, "frame") : nullptr;
		if (frame != nullptr) {
			u_json_get_float(u_json_get(frame, "eye_tracking_fps"), &eye_fps);
		}
		u_config_json_close(&json);
		eye_fps = std::clamp(eye_fps, 15.0f, 90.0f);
		U_LOG_I("Eye cameras at %.0f fps", eye_fps);
		cJSON_AddNumberToObject(cJSON_GetObjectItemCaseSensitive(layer, "steamvr"), "eyeTrackingRateMin", eye_fps);
		layers.push_back(layer);
	}
	analog_gain = debug_get_float_option_lh_default_brightness();
}

Settings::~Settings()
{
	cJSON_Delete(overrides);
	for (cJSON *layer : layers) {
		cJSON_Delete(layer);
	}
}

cJSON *
Settings::find(const char *section, const char *key)
{
	cJSON *value = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(overrides, section), key);
	for (size_t i = 0; value == nullptr && i < layers.size(); i++) {
		value = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(layers[i], section), key);
	}
	if (value == nullptr) {
		U_LOG_D("Setting %s/%s not found", section, key);
	}
	return value;
}

void
Settings::set(const char *section, const char *key, cJSON *value)
{
	std::lock_guard lk(mut);
	cJSON *sec = cJSON_GetObjectItemCaseSensitive(overrides, section);
	if (sec == nullptr) {
		sec = cJSON_AddObjectToObject(overrides, section);
	}
	if (cJSON_HasObjectItem(sec, key)) {
		cJSON_ReplaceItemInObjectCaseSensitive(sec, key, value);
	} else {
		cJSON_AddItemToObject(sec, key, value);
	}
}

const char *
Settings::GetSettingsErrorNameFromEnum(vr::EVRSettingsError eError)
{
	return nullptr;
}

void
Settings::SetBool(const char *pchSection, const char *pchSettingsKey, bool bValue, vr::EVRSettingsError *peError)
{
	set(pchSection, pchSettingsKey, cJSON_CreateBool(bValue));
	if (peError)
		*peError = vr::VRSettingsError_None;
}

void
Settings::SetInt32(const char *pchSection, const char *pchSettingsKey, int32_t nValue, vr::EVRSettingsError *peError)
{
	set(pchSection, pchSettingsKey, cJSON_CreateNumber(nValue));
	if (peError)
		*peError = vr::VRSettingsError_None;
}

void
Settings::SetFloat(const char *pchSection, const char *pchSettingsKey, float flValue, vr::EVRSettingsError *peError)
{
	if (pchSection == std::string_view{vr::k_pch_SteamVR_Section} && pchSettingsKey == "analogGain"sv) {
		analog_gain = flValue;
		context->add_vendor_event(vr::VREvent_SteamVRSectionSettingChanged);
	}
	set(pchSection, pchSettingsKey, cJSON_CreateNumber(flValue));
	if (peError)
		*peError = vr::VRSettingsError_None;
}

void
Settings::SetString(const char *pchSection,
                    const char *pchSettingsKey,
                    const char *pchValue,
                    vr::EVRSettingsError *peError)
{
	set(pchSection, pchSettingsKey, cJSON_CreateString(pchValue ? pchValue : ""));
	if (peError)
		*peError = vr::VRSettingsError_None;
}

bool
Settings::GetBool(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{
	if (pchSection == std::string_view{vr::k_pch_Power_Section} && pchSettingsKey == "powerOffOnExit"sv)
		return debug_get_bool_option_lh_standby_on_exit();

	std::lock_guard lk(mut);
	cJSON *value = find(pchSection, pchSettingsKey);
	if (peError)
		*peError = value ? vr::VRSettingsError_None : vr::VRSettingsError_ReadFailed;
	return cJSON_IsTrue(value) || (cJSON_IsNumber(value) && value->valuedouble != 0);
}

int32_t
Settings::GetInt32(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{
	std::lock_guard lk(mut);
	cJSON *value = find(pchSection, pchSettingsKey);
	if (peError)
		*peError = value ? vr::VRSettingsError_None : vr::VRSettingsError_ReadFailed;
	if (cJSON_IsBool(value))
		return cJSON_IsTrue(value);
	return cJSON_IsNumber(value) ? (int32_t)value->valuedouble : 0;
}

float
Settings::GetFloat(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{
	if (!strcmp(pchSection, vr::k_pch_SteamVR_Section)) {
		if (!strcmp(pchSettingsKey, "analogGain")) {
			return analog_gain;
		}
		if (!strcmp(pchSettingsKey, "ipd")) {
			// Inform the SteamVR driver we have 0 ipd (in case) it factors this into the eye matrix.
			return 0.f;
		}
	}
	std::lock_guard lk(mut);
	cJSON *value = find(pchSection, pchSettingsKey);
	if (peError)
		*peError = value ? vr::VRSettingsError_None : vr::VRSettingsError_ReadFailed;
	return cJSON_IsNumber(value) ? (float)value->valuedouble : 0.f;
}

void
Settings::GetString(const char *pchSection,
                    const char *pchSettingsKey,
                    char *pchValue,
                    uint32_t unValueLen,
                    vr::EVRSettingsError *peError)
{
	std::lock_guard lk(mut);
	cJSON *value = find(pchSection, pchSettingsKey);
	if (!cJSON_IsString(value)) {
		if (peError)
			*peError = vr::VRSettingsError_ReadFailed;
		if (pchValue && unValueLen > 0)
			pchValue[0] = '\0';
		return;
	}
	if (peError)
		*peError = vr::VRSettingsError_None;
	if (pchValue && unValueLen > 0) {
		std::strncpy(pchValue, value->valuestring, unValueLen - 1);
		pchValue[unValueLen - 1] = '\0';
	}
}

void
Settings::RemoveSection(const char *pchSection, vr::EVRSettingsError *peError)
{
	std::lock_guard lk(mut);
	cJSON_DeleteItemFromObjectCaseSensitive(overrides, pchSection);
}

void
Settings::RemoveKeyInSection(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError)
{
	std::lock_guard lk(mut);
	cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(overrides, pchSection), pchSettingsKey);
}
// NOLINTEND(bugprone-easily-swappable-parameters)
