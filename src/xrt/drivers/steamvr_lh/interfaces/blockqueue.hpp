// Copyright 2023, Shawn Wallace
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenVR IVRBlockQueue interface header.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#pragma once

#include "openvr_driver.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

/** Definitions missing from C++ header, present in C */
namespace vr {
inline const char *IVRBlockQueue_Version = "IVRBlockQueue_005";

typedef enum EBlockQueueError
{
	EBlockQueueError_BlockQueueError_None = 0,
	EBlockQueueError_BlockQueueError_QueueAlreadyExists = 1,
	EBlockQueueError_BlockQueueError_QueueNotFound = 2,
	EBlockQueueError_BlockQueueError_BlockNotAvailable = 3,
	EBlockQueueError_BlockQueueError_InvalidHandle = 4,
	EBlockQueueError_BlockQueueError_InvalidParam = 5,
	EBlockQueueError_BlockQueueError_ParamMismatch = 6,
	EBlockQueueError_BlockQueueError_InternalError = 7,
	EBlockQueueError_BlockQueueError_AlreadyInitialized = 8,
	EBlockQueueError_BlockQueueError_OperationIsServerOnly = 9,
	EBlockQueueError_BlockQueueError_TooManyConnections = 10,
} EBlockQueueError;

typedef enum EBlockQueueReadType
{
	EBlockQueueReadType_BlockQueueRead_Latest = 0,
	EBlockQueueReadType_BlockQueueRead_New = 1,
	EBlockQueueReadType_BlockQueueRead_Next = 2,
} EBlockQueueReadType;
} // namespace vr

/** This interface is missing in the C++ header but present in the C one, and the lighthouse driver requires it. */
struct BlockQueueShared;

// Shared-memory block queues, so another process (XRService through its vrclient.so) can connect to the same queues
class BlockQueue
{
public:
	BlockQueue() = default;
	BlockQueue(const BlockQueue &) = delete;
	~BlockQueue();

	virtual vr::EBlockQueueError
	Create(vr::PropertyContainerHandle_t *pulQueueHandle,
	       char *pchPath,
	       uint32_t unBlockDataSize,
	       uint32_t unBlockHeaderSize,
	       uint32_t unBlockCount,
	       uint32_t unFlags);
	virtual vr::EBlockQueueError
	Connect(vr::PropertyContainerHandle_t *pulQueueHandle, char *pchPath);
	virtual vr::EBlockQueueError
	Destroy(vr::PropertyContainerHandle_t ulQueueHandle);
	virtual vr::EBlockQueueError
	AcquireWriteOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
	                      vr::PropertyContainerHandle_t *pulBlockHandle,
	                      void **ppvBuffer);
	virtual vr::EBlockQueueError
	ReleaseWriteOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle, vr::PropertyContainerHandle_t ulBlockHandle);
	virtual vr::EBlockQueueError
	WaitAndAcquireReadOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
	                            vr::PropertyContainerHandle_t *pulBlockHandle,
	                            void **ppvBuffer,
	                            vr::EBlockQueueReadType eReadType,
	                            uint32_t unTimeoutMs);
	virtual vr::EBlockQueueError
	AcquireReadOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
	                     vr::PropertyContainerHandle_t *pulBlockHandle,
	                     void **ppvBuffer,
	                     vr::EBlockQueueReadType eReadType);
	virtual vr::EBlockQueueError
	ReleaseReadOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle, vr::PropertyContainerHandle_t ulBlockHandle);
	virtual vr::EBlockQueueError
	QueueHasReader(vr::PropertyContainerHandle_t ulQueueHandle, bool *pbHasReaders);

	//! Block handles double as property containers; their properties live in the block's header
	static bool
	is_block_handle(vr::PropertyContainerHandle_t handle)
	{
		return (handle & BLOCK_HANDLE_TAG) == BLOCK_HANDLE_TAG;
	}
	vr::ETrackedPropertyError
	write_block_properties(vr::PropertyContainerHandle_t block, vr::PropertyWrite_t *batch, uint32_t count);
	vr::ETrackedPropertyError
	read_block_properties(vr::PropertyContainerHandle_t block, vr::PropertyRead_t *batch, uint32_t count);
	//! Queue handles are property containers too: "/<name>/blockDataSize" etc., name = last path component
	vr::ETrackedPropertyError
	read_queue_properties(vr::PropertyContainerHandle_t queue, vr::PropertyRead_t *batch, uint32_t count);

private:
	// Keeps block handles apart from device property containers (small integers)
	static constexpr uint64_t BLOCK_HANDLE_TAG = 0x5f42510000000000; // "_BQ"

	struct Queue
	{
		BlockQueueShared *shared;
		size_t size;
		bool creator;
		uint64_t last_read; // sequence of the last block this handle read
		std::string name;   // last path component, e.g. "meshData"
	};
	std::mutex mut;
	std::vector<std::unique_ptr<Queue>> queues; // handle - 1 indexes this

	Queue *
	get(vr::PropertyContainerHandle_t handle);
	vr::EBlockQueueError
	acquire_read(Queue *q,
	             vr::PropertyContainerHandle_t queue_handle,
	             vr::PropertyContainerHandle_t *pulBlockHandle,
	             void **ppvBuffer,
	             vr::EBlockQueueReadType type);
	//! Header of the block behind a block handle, nullptr if it isn't one
	uint8_t *
	block_header(vr::PropertyContainerHandle_t block, uint32_t *size);
};
