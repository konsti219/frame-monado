#pragma once

#include "openvr_driver.h"
#include "passthrough.hpp"
#include <cjson/cJSON.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class ClientSettings : public vr::IVRSettings
{
public:
	ClientSettings()
	{
		const std::string home = getenv("HOME") ? getenv("HOME") : "";
		const std::string root = getenv("VR_OVERRIDE") ? getenv("VR_OVERRIDE") : "/opt/steamvr";
		for (const auto &path : {home + "/.config/openvr/config/steamvr.vrsettings",
		                         root + "/drivers/cv/resources/settings/default.vrsettings",
		                         root + "/resources/settings/default.vrsettings"}) {
			std::ifstream file(path);
			std::stringstream contents;
			contents << file.rdbuf();
			if (auto *layer = cJSON_Parse(contents.str().c_str())) {
				layers.push_back(layer);
			} else {
				passthrough_log("[settings] couldn't load %s\n", path.c_str());
			}
		}
		// SteamVR supplies this at runtime: 0/1 override XRService's color mode, 2 preserves it.
		if (getenv("SF_PASSTHROUGH_OFF") == nullptr) {
			SetInt32("camera", "roomView", 2, nullptr);
		}
	}

	~ClientSettings()
	{
		cJSON_Delete(overrides);
		for (auto *layer : layers)
			cJSON_Delete(layer);
	}

	const char *
	GetSettingsErrorNameFromEnum(vr::EVRSettingsError error) override
	{
		switch (error) {
		case vr::VRSettingsError_None: return "None";
		case vr::VRSettingsError_ReadFailed: return "ReadFailed";
		case vr::VRSettingsError_UnsetSettingHasNoDefault: return "UnsetSettingHasNoDefault";
		default: return "SettingsError";
		}
	}

	void
	SetBool(const char *section, const char *key, bool value, vr::EVRSettingsError *error) override
	{
		set(section, key, cJSON_CreateBool(value), error);
	}
	void
	SetInt32(const char *section, const char *key, int32_t value, vr::EVRSettingsError *error) override
	{
		set(section, key, cJSON_CreateNumber(value), error);
	}
	void
	SetFloat(const char *section, const char *key, float value, vr::EVRSettingsError *error) override
	{
		set(section, key, cJSON_CreateNumber(value), error);
	}
	void
	SetString(const char *section, const char *key, const char *value, vr::EVRSettingsError *error) override
	{
		set(section, key, cJSON_CreateString(value ? value : ""), error);
	}

	bool
	GetBool(const char *section, const char *key, vr::EVRSettingsError *error) override
	{
		std::lock_guard lock(mut);
		auto *value = find(section, key);
		finish(section, key, value, cJSON_IsBool(value), error);
		return cJSON_IsTrue(value);
	}
	int32_t
	GetInt32(const char *section, const char *key, vr::EVRSettingsError *error) override
	{
		std::lock_guard lock(mut);
		auto *value = find(section, key);
		finish(section, key, value, cJSON_IsNumber(value), error);
		return cJSON_IsNumber(value) ? value->valueint : 0;
	}
	float
	GetFloat(const char *section, const char *key, vr::EVRSettingsError *error) override
	{
		std::lock_guard lock(mut);
		auto *value = find(section, key);
		finish(section, key, value, cJSON_IsNumber(value), error);
		return cJSON_IsNumber(value) ? float(value->valuedouble) : 0;
	}
	void
	GetString(const char *section, const char *key, char *out, uint32_t size, vr::EVRSettingsError *error) override
	{
		std::lock_guard lock(mut);
		auto *value = find(section, key);
		finish(section, key, value, cJSON_IsString(value), error);
		if (out && size) {
			std::strncpy(out, cJSON_IsString(value) ? value->valuestring : "", size - 1);
			out[size - 1] = 0;
		}
	}

	void
	RemoveSection(const char *section, vr::EVRSettingsError *error) override
	{
		std::lock_guard lock(mut);
		cJSON_DeleteItemFromObjectCaseSensitive(overrides, section);
		if (error)
			*error = vr::VRSettingsError_None;
	}
	void
	RemoveKeyInSection(const char *section, const char *key, vr::EVRSettingsError *error) override
	{
		std::lock_guard lock(mut);
		cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(overrides, section), key);
		if (error)
			*error = vr::VRSettingsError_None;
	}

private:
	std::mutex mut;
	cJSON *overrides = cJSON_CreateObject();
	std::vector<cJSON *> layers;
	std::unordered_map<std::string, std::string> logged;

	cJSON *
	find(const char *section, const char *key)
	{
		auto *value =
		    cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(overrides, section), key);
		for (auto *layer : layers) {
			if (value)
				break;
			value = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(layer, section), key);
		}
		return value;
	}
	void
	finish(const char *section, const char *key, cJSON *value, bool valid, vr::EVRSettingsError *error)
	{
		if (error)
			*error = valid   ? vr::VRSettingsError_None
			         : value ? vr::VRSettingsError_ReadFailed
			                 : vr::VRSettingsError_UnsetSettingHasNoDefault;
		char *json = value ? cJSON_PrintUnformatted(value) : nullptr;
		const std::string text = json ? json : "<missing>";
		cJSON_free(json);
		const std::string name = std::string(section) + "/" + key;
		if (logged.find(name) == logged.end() || logged[name] != text) {
			passthrough_log("[settings] %s = %s%s\n", name.c_str(), text.c_str(),
			                valid ? "" : " (read failed)");
			logged[name] = text;
		}
	}
	void
	set(const char *section, const char *key, cJSON *value, vr::EVRSettingsError *error)
	{
		std::lock_guard lock(mut);
		auto *sec = cJSON_GetObjectItemCaseSensitive(overrides, section);
		if (!sec)
			sec = cJSON_AddObjectToObject(overrides, section);
		if (cJSON_HasObjectItem(sec, key))
			cJSON_ReplaceItemInObjectCaseSensitive(sec, key, value);
		else
			cJSON_AddItemToObject(sec, key, value);
		if (error)
			*error = vr::VRSettingsError_None;
	}
};
