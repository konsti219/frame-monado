// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief In-process IVRMailbox_002 (private SteamVR interface), used by the Steam Frame's driver_cv.
 * @ingroup drv_steamvr_lh
 */

#include "mailbox.hpp"

#include <cstring>

#define MB_DEBUG(...) U_LOG_IFL_D(log_level, __VA_ARGS__)

Mailbox::Error
Mailbox::RegisterMailbox(const char *pchName, uint64_t *pHandle, char *pchFullName, uint32_t unFullNameLen)
{
	if (pchName == nullptr || pHandle == nullptr) {
		return InvalidHandle;
	}
	std::lock_guard lk(mut);
	uint64_t handle = next_handle++;
	names[handle] = pchName;
	queues.try_emplace(pchName);
	*pHandle = handle;
	if (pchFullName != nullptr && unFullNameLen > 0) {
		std::strncpy(pchFullName, pchName, unFullNameLen - 1);
		pchFullName[unFullNameLen - 1] = '\0';
	}
	MB_DEBUG("Registered mailbox %s as %lu", pchName, (unsigned long)handle);
	return None;
}

Mailbox::Error
Mailbox::UnregisterMailbox(uint64_t ulHandle)
{
	std::lock_guard lk(mut);
	return names.erase(ulHandle) ? None : InvalidHandle;
}

Mailbox::Error
Mailbox::SendMailboxMessage(uint64_t ulFrom, const char *pchTo, const char *pchMessage)
{
	std::lock_guard lk(mut);
	MB_DEBUG("Mailbox message to %s: %s", pchTo, pchMessage);
	auto queue = queues.find(pchTo);
	if (queue != queues.end()) {
		queue->second.emplace_back(pchMessage);
	}
	return None;
}

Mailbox::Error
Mailbox::GetNextMessage(uint64_t ulHandle, char *pchBuffer, uint32_t unBufferLen, uint32_t *punRequiredLen)
{
	std::lock_guard lk(mut);
	auto name = names.find(ulHandle);
	if (name == names.end()) {
		return InvalidHandle;
	}
	std::deque<std::string> &queue = queues[name->second];
	if (queue.empty()) {
		return NoMessage;
	}
	const std::string &message = queue.front();
	if (punRequiredLen != nullptr) {
		*punRequiredLen = message.size() + 1;
	}
	if (pchBuffer == nullptr || unBufferLen < message.size() + 1) {
		return BufferTooSmall;
	}
	std::memcpy(pchBuffer, message.c_str(), message.size() + 1);
	queue.pop_front();
	return None;
}

bool
Mailbox::post(const std::string &to, const std::string &message)
{
	std::lock_guard lk(mut);
	auto queue = queues.find(to);
	if (queue == queues.end()) {
		return false;
	}
	queue->second.push_back(message);
	return true;
}
