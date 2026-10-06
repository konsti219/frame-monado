// Copyright 2023, Shawn Wallace
// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenVR IVRPaths interface implementation.
 * @ingroup drv_steamvr_lh
 */

#include "paths.hpp"

#include <cstring>

vr::PathHandle_t
Paths::handle_for(const char *path)
{
	// FNV-1a; 0x1xxxxxxx keeps them in the range of vrserver's path property ids
	uint32_t hash = 2166136261u;
	for (const char *c = path; *c; c++) {
		hash = (hash ^ (uint8_t)*c) * 16777619u;
	}
	return 0x10000000u | (hash & 0x0fffffffu);
}

vr::ETrackedDeviceProperty
Paths::property_for(vr::PathHandle_t handle)
{
	// As vrserver's ReadSingleNormalPathFast does it
	return (vr::ETrackedDeviceProperty)((uint32_t)handle | ((uint32_t)(handle >> 32) & 0xf0000000u));
}

vr::ETrackedPropertyError
Paths::ReadPathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathRead_t *pBatch, uint32_t unBatchEntryCount)
{
	for (uint32_t i = 0; i < unBatchEntryCount; i++) {
		vr::PathRead_t &r = pBatch[i];
		vr::PathHandle_t path = r.pszPath ? handle_for(r.pszPath) : r.ulPath;
		vr::PropertyRead_t prop{property_for(path), r.pvBuffer, r.unBufferSize, 0, 0, vr::TrackedProp_Success};
		properties->ReadPropertyBatch(ulRootHandle, &prop, 1);
		r.unTag = prop.unTag;
		r.unRequiredBufferSize = prop.unRequiredBufferSize;
		r.eError = prop.eError;
	}
	return vr::TrackedProp_Success;
}

vr::ETrackedPropertyError
Paths::WritePathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathWrite_t *pBatch, uint32_t unBatchEntryCount)
{
	for (uint32_t i = 0; i < unBatchEntryCount; i++) {
		vr::PathWrite_t &w = pBatch[i];
		vr::PathHandle_t path = w.pszPath ? handle_for(w.pszPath) : w.ulPath;
		vr::PropertyWrite_t prop{property_for(path), w.writeType,    w.eSetError,
		                         w.pvBuffer,         w.unBufferSize, w.unTag,
		                         vr::TrackedProp_Success};
		properties->WritePropertyBatch(ulRootHandle, &prop, 1);
		w.eError = prop.eError;
	}
	return vr::TrackedProp_Success;
}

vr::ETrackedPropertyError
Paths::StringToHandle(vr::PathHandle_t *pHandle, const char *pchPath)
{
	if (pHandle == nullptr || pchPath == nullptr) {
		return vr::TrackedProp_InvalidOperation;
	}
	*pHandle = handle_for(pchPath);
	std::lock_guard lk(mut);
	names.emplace(*pHandle, pchPath);
	return vr::TrackedProp_Success;
}

vr::ETrackedPropertyError
Paths::HandleToString(vr::PathHandle_t pHandle, char *pchBuffer, uint32_t unBufferSize, uint32_t *punBufferSizeUsed)
{
	std::lock_guard lk(mut);
	auto name = names.find(pHandle);
	if (name == names.end()) {
		return vr::TrackedProp_UnknownProperty;
	}
	if (punBufferSizeUsed) {
		*punBufferSizeUsed = name->second.size() + 1;
	}
	if (pchBuffer == nullptr || unBufferSize < name->second.size() + 1) {
		return vr::TrackedProp_BufferTooSmall;
	}
	std::memcpy(pchBuffer, name->second.c_str(), name->second.size() + 1);
	return vr::TrackedProp_Success;
}
