// Copyright 2023, Shawn Wallace
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief SteamVR driver context - implements xrt_tracking_origin and IVRDriverContext.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#pragma once

#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <optional>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "openvr_driver.h"
#include "util/u_logging.h"

#include "settings.hpp"
#include "resources.hpp"
#include "iobuffer.hpp"
#include "driver_manager.hpp"
#include "server.hpp"
#include "blockqueue.hpp"
#include "paths.hpp"
#include "mailbox.hpp"

#include "xrt/xrt_tracking.h"

struct xrt_input;
class Device;
class ControllerDevice;
/*!
 * IVRPropertiesInternal_001 (private): IVRProperties' 4 methods first, then unknown ones. driver_cv writes block
 * queue block properties through it.
 */
class PropertiesInternal : public vr::IVRProperties
{
public:
	explicit PropertiesInternal(vr::IVRProperties &properties) : p(properties) {}

	vr::ETrackedPropertyError
	ReadPropertyBatch(vr::PropertyContainerHandle_t h, vr::PropertyRead_t *b, uint32_t n) override
	{
		return p.ReadPropertyBatch(h, b, n);
	}
	vr::ETrackedPropertyError
	WritePropertyBatch(vr::PropertyContainerHandle_t h, vr::PropertyWrite_t *b, uint32_t n) override
	{
		return p.WritePropertyBatch(h, b, n);
	}
	const char *
	GetPropErrorNameFromEnum(vr::ETrackedPropertyError e) override
	{
		return p.GetPropErrorNameFromEnum(e);
	}
	vr::PropertyContainerHandle_t
	TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t d) override
	{
		return p.TrackedDeviceToPropertyContainer(d);
	}

#define PROPERTIES_INTERNAL_UNKNOWN(n)                                                                                 \
	virtual uint64_t Unknown##n(uint64_t a, uint64_t b, uint64_t c)                                               \
	{                                                                                                              \
		U_LOG_W("[stub] IVRPropertiesInternal_001 slot " #n " (%#lx, %#lx, %#lx)", (unsigned long)a,           \
		        (unsigned long)b, (unsigned long)c);                                                         \
		return 0;                                                                                      \
	}
	PROPERTIES_INTERNAL_UNKNOWN(4)
	PROPERTIES_INTERNAL_UNKNOWN(5)
	PROPERTIES_INTERNAL_UNKNOWN(6)
	PROPERTIES_INTERNAL_UNKNOWN(7)
	PROPERTIES_INTERNAL_UNKNOWN(8)
	PROPERTIES_INTERNAL_UNKNOWN(9)
	PROPERTIES_INTERNAL_UNKNOWN(10)
	PROPERTIES_INTERNAL_UNKNOWN(11)
	PROPERTIES_INTERNAL_UNKNOWN(12)
	PROPERTIES_INTERNAL_UNKNOWN(13)
	PROPERTIES_INTERNAL_UNKNOWN(14)
	PROPERTIES_INTERNAL_UNKNOWN(15)
	PROPERTIES_INTERNAL_UNKNOWN(16)
	PROPERTIES_INTERNAL_UNKNOWN(17)
	PROPERTIES_INTERNAL_UNKNOWN(18)
	PROPERTIES_INTERNAL_UNKNOWN(19)
#undef PROPERTIES_INTERNAL_UNKNOWN

private:
	vr::IVRProperties &p;
};

class Context final : public xrt_tracking_origin,
                      public vr::IVRDriverContext,
                      public vr::IVRServerDriverHost,
                      public vr::IVRDriverInput,
                      public vr::IVRProperties,
                      public vr::IVRDriverLog,
                      public std::enable_shared_from_this<Context>

{
public:
	Settings settings;
	SettingsInternal settings_internal{settings};
	PropertiesInternal properties_internal{*this};

	struct Vec2Components
	{
		vr::VRInputComponentHandle_t x;
		vr::VRInputComponentHandle_t y;
	};

	/*
	 * All data types are are locked by `devices_mut`
	 */
	struct
	{
		vr::VRInputComponentHandle_t next_handle;
		std::unordered_map<vr::VRInputComponentHandle_t, xrt_input *> handle_to_input;
		std::unordered_map<vr::VRInputComponentHandle_t, Vec2Components *> vec2_inputs;
		std::unordered_map<xrt_input *, std::unique_ptr<Vec2Components>> vec2_input_to_components;
		std::unordered_map<vr::VRInputComponentHandle_t, ControllerDevice *> skeleton_to_controller;
	} input;

private:
	Resources resources;
	IOBuffer iobuf;
	DriverManager man;
	Server server;
	BlockQueue blockqueue;
	Paths paths;
	Mailbox mailbox;

	// Install and user config folders of the loaded driver, read through the driver handle
	std::string driver_install_path;
	std::string driver_user_config_path;

	uint64_t current_frame{0};

	struct Event
	{
		std::chrono::steady_clock::time_point insert_time;
		vr::VREvent_t inner;
	};
	std::deque<Event> events;
	size_t events_tail{0};
	std::mutex event_queue_mut;
	std::condition_variable event_popped;

	// Separate from the device containers (HMD is 1, controllers follow)
	static constexpr vr::PropertyContainerHandle_t DRIVER_HANDLE = 0x10000;

	vr::ETrackedPropertyError
	read_driver_properties(vr::PropertyRead_t *batch, uint32_t count);

	Device *
	prop_container_to_device(vr::PropertyContainerHandle_t handle);

	vr::EVRInputError
	create_component_common(vr::PropertyContainerHandle_t container,
	                        const char *name,
	                        vr::VRInputComponentHandle_t *handle);

	xrt_input *
	update_component_common_locked(vr::VRInputComponentHandle_t handle, double offset);

	bool
	setup_hmd(const char *serial, vr::ITrackedDeviceServerDriver *driver);

	bool
	setup_controller(const char *serial, vr::ITrackedDeviceServerDriver *driver);
	std::vector<vr::IServerTrackedDeviceProvider *> providers;

public:
	Context(const std::string &steam_install, const std::string &steamvr_install, u_logging_level level);

	// These are owned by Monado, context is destroyed when these are destroyed
	std::mutex devices_mut{};
	class HmdDevice *hmd{nullptr};
	class ControllerDevice *controller[16]{nullptr};
	bool controller_ready[16]{false};
	bool allow_late_controllers{false};
	bool shutting_down{false};
	void retire_controllers();

	bool in_setup{true};
	const u_logging_level log_level;

	void
	wait_for_discover();

	void
	extend_discover();

private:
	std::condition_variable discover_cv;
	std::chrono::steady_clock::time_point discover_end_time;
	//! Devices published to `hmd`/`controller` that the driver has not finished activating (`devices_mut`).
	size_t devices_in_setup{0};
	std::atomic<bool> frame_thread_run;
	std::binary_semaphore frame_thread_event{0};
	std::thread frame_thread;

	void
	add_event_locked(vr::VREvent_t event);

public:
	~Context();

	[[nodiscard]] static std::shared_ptr<Context>
	create(const std::string &steam_install,
	       const std::string &steamvr_install,
	       std::vector<vr::IServerTrackedDeviceProvider *> providers);

	size_t
	add_haptic_event(vr::VREvent_HapticVibration_t event, size_t old_event_index);

	void
	add_vendor_event(vr::EVREventType type, const vr::VREvent_Data_t &data = {})
	{
		VendorSpecificEvent(0, type, data, 0);
	}

	void
	Log(const char *pchLogMessage) override;

	/***** IVRDriverContext methods *****/

	void *
	GetGenericInterface(const char *pchInterfaceVersion, vr::EVRInitError *peError) override;

	vr::DriverHandle_t
	GetDriverHandle() override;

	/***** IVRServerDriverHost methods *****/
	bool
	TrackedDeviceAdded(const char *pchDeviceSerialNumber,
	                   vr::ETrackedDeviceClass eDeviceClass,
	                   vr::ITrackedDeviceServerDriver *pDriver) override;

	void
	TrackedDevicePoseUpdated(uint32_t unWhichDevice,
	                         const vr::DriverPose_t &newPose,
	                         uint32_t unPoseStructSize) override;

	void
	VsyncEvent(double vsyncTimeOffsetSeconds) override;

	void
	VendorSpecificEvent(uint32_t unWhichDevice,
	                    vr::EVREventType eventType,
	                    const vr::VREvent_Data_t &eventData,
	                    double eventTimeOffset) override;

	bool
	IsExiting() override;

	bool
	PollNextEvent(vr::VREvent_t *pEvent, uint32_t uncbVREvent) override;

	void
	GetRawTrackedDevicePoses(float fPredictedSecondsFromNow,
	                         vr::TrackedDevicePose_t *pTrackedDevicePoseArray,
	                         uint32_t unTrackedDevicePoseArrayCount) override;

	void
	RequestRestart(const char *pchLocalizedReason,
	               const char *pchExecutableToStart,
	               const char *pchArguments,
	               const char *pchWorkingDirectory) override;

	uint32_t
	GetFrameTimings(vr::Compositor_FrameTiming *pTiming, uint32_t nFrames) override;

	void
	SetDisplayEyeToHead(uint32_t unWhichDevice,
	                    const vr::HmdMatrix34_t &eyeToHeadLeft,
	                    const vr::HmdMatrix34_t &eyeToHeadRight) override;

	void
	SetDisplayProjectionRaw(uint32_t unWhichDevice,
	                        const vr::HmdRect2_t &eyeLeft,
	                        const vr::HmdRect2_t &eyeRight) override;

	void
	SetRecommendedRenderTargetSize(uint32_t unWhichDevice, uint32_t nWidth, uint32_t nHeight) override;

	/***** IVRDriverInput methods *****/

	vr::EVRInputError
	CreateBooleanComponent(vr::PropertyContainerHandle_t ulContainer,
	                       const char *pchName,
	                       vr::VRInputComponentHandle_t *pHandle) override;

	vr::EVRInputError
	UpdateBooleanComponent(vr::VRInputComponentHandle_t ulComponent, bool bNewValue, double fTimeOffset) override;

	vr::EVRInputError
	CreateScalarComponent(vr::PropertyContainerHandle_t ulContainer,
	                      const char *pchName,
	                      vr::VRInputComponentHandle_t *pHandle,
	                      vr::EVRScalarType eType,
	                      vr::EVRScalarUnits eUnits) override;

	vr::EVRInputError
	UpdateScalarComponent(vr::VRInputComponentHandle_t ulComponent, float fNewValue, double fTimeOffset) override;

	vr::EVRInputError
	CreateHapticComponent(vr::PropertyContainerHandle_t ulContainer,
	                      const char *pchName,
	                      vr::VRInputComponentHandle_t *pHandle) override;

	vr::EVRInputError
	CreateSkeletonComponent(vr::PropertyContainerHandle_t ulContainer,
	                        const char *pchName,
	                        const char *pchSkeletonPath,
	                        const char *pchBasePosePath,
	                        vr::EVRSkeletalTrackingLevel eSkeletalTrackingLevel,
	                        const vr::VRBoneTransform_t *pGripLimitTransforms,
	                        uint32_t unGripLimitTransformCount,
	                        vr::VRInputComponentHandle_t *pHandle) override;

	vr::EVRInputError
	UpdateSkeletonComponent(vr::VRInputComponentHandle_t ulComponent,
	                        vr::EVRSkeletalMotionRange eMotionRange,
	                        const vr::VRBoneTransform_t *pTransforms,
	                        uint32_t unTransformCount) override;

	vr::EVRInputError
	CreatePoseComponent(vr::PropertyContainerHandle_t ulContainer,
	                    const char *pchName,
	                    vr::VRInputComponentHandle_t *pHandle) override;

	vr::EVRInputError
	UpdatePoseComponent(vr::VRInputComponentHandle_t ulComponent,
	                    const vr::HmdMatrix34_t *pMatPoseOffset,
	                    double fTimeOffset) override;

	vr::EVRInputError
	CreateEyeTrackingComponent(vr::PropertyContainerHandle_t ulContainer,
	                           const char *pchName,
	                           vr::VRInputComponentHandle_t *pHandle) override;

	vr::EVRInputError
	UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t ulComponent,
	                           const vr::VREyeTrackingData_t *pEyeTrackingData,
	                           double fTimeOffset) override;

	/***** IVRProperties methods *****/

	vr::ETrackedPropertyError
	ReadPropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle,
	                  vr::PropertyRead_t *pBatch,
	                  uint32_t unBatchEntryCount) override;

	vr::ETrackedPropertyError
	WritePropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle,
	                   vr::PropertyWrite_t *pBatch,
	                   uint32_t unBatchEntryCount) override;

	const char *
	GetPropErrorNameFromEnum(vr::ETrackedPropertyError error) override;

	vr::PropertyContainerHandle_t
	TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t nDevice) override;
};
