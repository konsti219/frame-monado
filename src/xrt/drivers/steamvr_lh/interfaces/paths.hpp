// Copyright 2023, Shawn Wallace
// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenVR IVRPaths interface header.
 * @ingroup drv_steamvr_lh
 */

#pragma once

#include "openvr_driver.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace vr {
inline const char *IVRPaths_Version = "IVRPaths_001";
typedef uint64_t PathHandle_t;

// Not in the public headers; layouts from vrserver's CPathManagerBase
struct PathRead_t
{
	PathHandle_t ulPath;
	void *pvBuffer;
	uint32_t unBufferSize;
	PropertyTypeTag_t unTag;
	uint32_t unRequiredBufferSize;
	ETrackedPropertyError eError;
	const char *pszPath;
};
static_assert(sizeof(PathRead_t) == 40);

struct PathWrite_t
{
	PathHandle_t ulPath;
	EPropertyWriteType writeType;
	ETrackedPropertyError eSetError;
	void *pvBuffer;
	uint32_t unBufferSize;
	PropertyTypeTag_t unTag;
	ETrackedPropertyError eError;
	const char *pszPath;
	uint64_t unknown; // IVRPaths_002 entries are 56 bytes
};
static_assert(sizeof(PathWrite_t) == 56);
} // namespace vr

/*!
 * Paths are properties addressed by name: a path handle carries a property id, and path reads and writes become
 * property reads and writes on the same container (that's how vrserver does it). Handles are a hash of the path,
 * so a separate process (XRService through its vrclient.so) derives the same ones.
 */
class Paths
{
public:
	explicit Paths(vr::IVRProperties *properties) : properties(properties) {}

	virtual vr::ETrackedPropertyError
	ReadPathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathRead_t *pBatch, uint32_t unBatchEntryCount);
	virtual vr::ETrackedPropertyError
	WritePathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathWrite_t *pBatch, uint32_t unBatchEntryCount);
	virtual vr::ETrackedPropertyError
	StringToHandle(vr::PathHandle_t *pHandle, const char *pchPath);
	virtual vr::ETrackedPropertyError
	HandleToString(vr::PathHandle_t pHandle, char *pchBuffer, uint32_t unBufferSize, uint32_t *punBufferSizeUsed);

	static vr::PathHandle_t
	handle_for(const char *path);
	static vr::ETrackedDeviceProperty
	property_for(vr::PathHandle_t handle);

private:
	vr::IVRProperties *properties;
	std::mutex mut;
	std::unordered_map<vr::PathHandle_t, std::string> names;
};
