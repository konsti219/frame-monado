// Copyright 2023, Shawn Wallace
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenVR IVRSettings interface header.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#pragma once

#include "openvr_driver.h"
#include "util/u_json.hpp"
#include "util/u_logging.h"

#include <mutex>
#include <string>
#include <vector>

struct cJSON;

class Context;

class Settings : public vr::IVRSettings
{
private:
	// Lookup order: runtime overrides, user settings, driver defaults, SteamVR defaults
	cJSON *overrides;
	std::vector<cJSON *> layers;
	std::mutex mut;

	cJSON *
	find(const char *section, const char *key);
	void
	set(const char *section, const char *key, cJSON *value);

	Context *context;

	float analog_gain;

public:
	Settings(const std::string &steam_install, const std::string &steamvr_install, Context *context);
	~Settings();

	const char *
	GetSettingsErrorNameFromEnum(vr::EVRSettingsError eError) override;

	void
	SetBool(const char *pchSection,
	        const char *pchSettingsKey,
	        bool bValue,
	        vr::EVRSettingsError *peError = nullptr) override;
	void
	SetInt32(const char *pchSection,
	         const char *pchSettingsKey,
	         int32_t nValue,
	         vr::EVRSettingsError *peError = nullptr) override;
	void
	SetFloat(const char *pchSection,
	         const char *pchSettingsKey,
	         float flValue,
	         vr::EVRSettingsError *peError = nullptr) override;
	void
	SetString(const char *pchSection,
	          const char *pchSettingsKey,
	          const char *pchValue,
	          vr::EVRSettingsError *peError = nullptr) override;

	// Users of the system need to provide a proper default in default.vrsettings in the resources/settings/
	// directory of either the runtime or the driver_xxx directory. Otherwise the default will be false, 0, 0.0 or
	// ""
	bool
	GetBool(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError = nullptr) override;
	int32_t
	GetInt32(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError = nullptr) override;
	float
	GetFloat(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError = nullptr) override;
	void
	GetString(const char *pchSection,
	          const char *pchSettingsKey,
	          VR_OUT_STRING() char *pchValue,
	          uint32_t unValueLen,
	          vr::EVRSettingsError *peError = nullptr) override;

	void
	RemoveSection(const char *pchSection, vr::EVRSettingsError *peError = nullptr) override;
	void
	RemoveKeyInSection(const char *pchSection,
	                   const char *pchSettingsKey,
	                   vr::EVRSettingsError *peError = nullptr) override;
};

/*!
 * IVRSettingsInternal_001 (private): IVRSettings' 11 methods first, then unknown ones. driver_cv reads some
 * settings, e.g. controllerTracking3, through it.
 */
class SettingsInternal : public vr::IVRSettings
{
public:
	explicit SettingsInternal(Settings &settings) : s(settings) {}

	const char *
	GetSettingsErrorNameFromEnum(vr::EVRSettingsError e) override
	{
		return s.GetSettingsErrorNameFromEnum(e);
	}
	void
	SetBool(const char *a, const char *b, bool v, vr::EVRSettingsError *e) override
	{
		s.SetBool(a, b, v, e);
	}
	void
	SetInt32(const char *a, const char *b, int32_t v, vr::EVRSettingsError *e) override
	{
		s.SetInt32(a, b, v, e);
	}
	void
	SetFloat(const char *a, const char *b, float v, vr::EVRSettingsError *e) override
	{
		s.SetFloat(a, b, v, e);
	}
	void
	SetString(const char *a, const char *b, const char *v, vr::EVRSettingsError *e) override
	{
		s.SetString(a, b, v, e);
	}
	bool
	GetBool(const char *a, const char *b, vr::EVRSettingsError *e) override
	{
		return s.GetBool(a, b, e);
	}
	int32_t
	GetInt32(const char *a, const char *b, vr::EVRSettingsError *e) override
	{
		return s.GetInt32(a, b, e);
	}
	float
	GetFloat(const char *a, const char *b, vr::EVRSettingsError *e) override
	{
		return s.GetFloat(a, b, e);
	}
	void
	GetString(const char *a, const char *b, char *v, uint32_t len, vr::EVRSettingsError *e) override
	{
		s.GetString(a, b, v, len, e);
	}
	void
	RemoveSection(const char *a, vr::EVRSettingsError *e) override
	{
		s.RemoveSection(a, e);
	}
	void
	RemoveKeyInSection(const char *a, const char *b, vr::EVRSettingsError *e) override
	{
		s.RemoveKeyInSection(a, b, e);
	}

#define SETTINGS_INTERNAL_UNKNOWN(n)                                                                                   \
	virtual uint64_t Unknown##n(uint64_t a, uint64_t b, uint64_t c)                                               \
	{                                                                                                              \
		U_LOG_W("[stub] IVRSettingsInternal_001 slot " #n " (%#lx, %#lx, %#lx)", (unsigned long)a,             \
		        (unsigned long)b, (unsigned long)c);                                                         \
		return 0;                                                                                      \
	}
	SETTINGS_INTERNAL_UNKNOWN(11)
	SETTINGS_INTERNAL_UNKNOWN(12)
	SETTINGS_INTERNAL_UNKNOWN(13)
	SETTINGS_INTERNAL_UNKNOWN(14)
	SETTINGS_INTERNAL_UNKNOWN(15)
	SETTINGS_INTERNAL_UNKNOWN(16)
	SETTINGS_INTERNAL_UNKNOWN(17)
	SETTINGS_INTERNAL_UNKNOWN(18)
	SETTINGS_INTERNAL_UNKNOWN(19)
	SETTINGS_INTERNAL_UNKNOWN(20)
	SETTINGS_INTERNAL_UNKNOWN(21)
	SETTINGS_INTERNAL_UNKNOWN(22)
	SETTINGS_INTERNAL_UNKNOWN(23)
	SETTINGS_INTERNAL_UNKNOWN(24)
	SETTINGS_INTERNAL_UNKNOWN(25)
	SETTINGS_INTERNAL_UNKNOWN(26)
	SETTINGS_INTERNAL_UNKNOWN(27)
	SETTINGS_INTERNAL_UNKNOWN(28)
	SETTINGS_INTERNAL_UNKNOWN(29)
	SETTINGS_INTERNAL_UNKNOWN(30)
#undef SETTINGS_INTERNAL_UNKNOWN

private:
	Settings &s;
};
