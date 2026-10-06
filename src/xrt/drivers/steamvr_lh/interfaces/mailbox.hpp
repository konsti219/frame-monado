// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief In-process IVRMailbox_002 (private SteamVR interface), used by the Steam Frame's driver_cv.
 * @ingroup drv_steamvr_lh
 */

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "util/u_logging.h"

// Method order and return codes from vrserver's CVRMailboxServer
class Mailbox
{
public:
	enum Error : int32_t
	{
		None = 0,
		NoMessage = 1,
		BufferTooSmall = 2,
		InvalidHandle = 3,
	};

	explicit Mailbox(u_logging_level level) : log_level(level) {}

	virtual Error
	RegisterMailbox(const char *pchName, uint64_t *pHandle, char *pchFullName, uint32_t unFullNameLen);
	virtual Error
	UnregisterMailbox(uint64_t ulHandle);
	virtual Error
	SendMailboxMessage(uint64_t ulFrom, const char *pchTo, const char *pchMessage);
	virtual Error
	GetNextMessage(uint64_t ulHandle, char *pchBuffer, uint32_t unBufferLen, uint32_t *punRequiredLen);

	//! Host-side send, for messages SteamVR itself would send; false if nobody registered @p to
	bool
	post(const std::string &to, const std::string &message);

private:
	u_logging_level log_level;
	std::mutex mut;
	uint64_t next_handle{1};
	std::unordered_map<uint64_t, std::string> names;
	std::unordered_map<std::string, std::deque<std::string>> queues;
};
