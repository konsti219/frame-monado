// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metrics saving functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#include "os/os_threading.h"

#include "util/u_metrics.h"
#include "util/u_debug.h"

#include "monado_metrics.pb.h"
#include "pb_encode.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#if defined(XRT_OS_LINUX)
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(XRT_OS_WINDOWS)
#include <io.h>
#endif

#define VERSION_MAJOR 1
#define VERSION_MINOR 1

#define BUFFER_SIZE (monado_metrics_Record_size + 10)

enum u_metrics_sink_type
{
	U_METRICS_SINK_TYPE_REGULAR_FILE,
	U_METRICS_SINK_TYPE_SOCKET,
};

struct u_metrics_sink
{
	enum u_metrics_sink_type type;
	union {
		FILE *file;
#if defined(XRT_OS_LINUX)
		xrt_file_handle_t socket;
#endif
	};
};

static struct u_metrics_sink *g_metrics_sinks = NULL;
static size_t g_metrics_sink_count = 0;
static struct os_mutex g_metrics_mutex;
static bool g_metrics_initialized = false;

DEBUG_GET_ONCE_OPTION(metrics_file, "XRT_METRICS_FILE", NULL)
DEBUG_GET_ONCE_BOOL_OPTION(metrics_early_flush, "XRT_METRICS_EARLY_FLUSH", false)



/*
 *
 * Helper functions.
 *
 */

static void
encode_record(monado_metrics_Record *r, uint8_t out_buffer[BUFFER_SIZE], size_t *out_size)
{
	pb_ostream_t stream = pb_ostream_from_buffer(out_buffer, BUFFER_SIZE);
	bool ret = pb_encode_submessage(&stream, &monado_metrics_Record_msg, r);
	if (!ret) {
		U_LOG_E("Failed to encode metrics message!");
		*out_size = 0;
		return;
	}

	*out_size = stream.bytes_written;
}

static bool
write_buffer_to_regular_file(uint8_t *buffer, size_t size, FILE *file)
{
	return fwrite(buffer, size, 1, file) == 1;
}

#if defined(XRT_OS_LINUX)
static bool
write_buffer_to_socket(uint8_t *buffer, size_t size, int socket)
{
	return (size_t)send(socket, buffer, size, MSG_NOSIGNAL) == size;
}
#endif

static bool
write_buffer_to_sink_locked(uint8_t *buffer, size_t size, struct u_metrics_sink *sink)
{
	switch (sink->type) {
	case U_METRICS_SINK_TYPE_REGULAR_FILE: return write_buffer_to_regular_file(buffer, size, sink->file);
#if defined(XRT_OS_LINUX)
	case U_METRICS_SINK_TYPE_SOCKET: return write_buffer_to_socket(buffer, size, sink->socket);
#endif
	default: assert(0 && "unhandled enum value");
	}
	return false;
}

static bool
write_record_to_sink_locked(monado_metrics_Record *r, struct u_metrics_sink *sink)
{
	uint8_t buffer[BUFFER_SIZE];
	size_t size = 0;

	encode_record(r, buffer, &size);

	if (size < 1) {
		return false;
	}

	return write_buffer_to_sink_locked(buffer, size, sink);
}

static bool
write_version_to_sink_locked(uint32_t major, uint32_t minor, struct u_metrics_sink *sink)
{
	monado_metrics_Record record = monado_metrics_Record_init_default;

	// Select which filed is used.
	record.which_record = monado_metrics_Record_version_tag;
	record.record.version.major = major;
	record.record.version.minor = minor;

	return write_record_to_sink_locked(&record, sink);
}

static void
close_sink_regular_file(struct u_metrics_sink *sink)
{
	if (!sink->file) {
		return;
	}

	fclose(sink->file);
	sink->file = NULL;
}

#if defined(XRT_OS_LINUX)
static void
close_sink_socket(struct u_metrics_sink *sink)
{
	if (sink->socket == XRT_FILE_HANDLE_INVALID) {
		return;
	}

	close(sink->socket);
	sink->socket = XRT_FILE_HANDLE_INVALID;
}
#endif

static void
close_sink_stream_locked(struct u_metrics_sink *sink)
{
	switch (sink->type) {
	case U_METRICS_SINK_TYPE_REGULAR_FILE: close_sink_regular_file(sink); break;
#if defined(XRT_OS_LINUX)
	case U_METRICS_SINK_TYPE_SOCKET: close_sink_socket(sink); break;
#endif
	default: assert(0 && "unhandled enum value");
	}
}

static void
close_sink_locked(struct u_metrics_sink *sink)
{
	close_sink_stream_locked(sink);

	size_t sink_num = sink - g_metrics_sinks;
	memcpy(g_metrics_sinks + sink_num, g_metrics_sinks + (sink_num + 1),
	       sizeof(struct u_metrics_sink) * (g_metrics_sink_count - (sink_num + 1)));
	g_metrics_sink_count--;

	if (!g_metrics_sink_count) {
		free(g_metrics_sinks);
		g_metrics_sinks = NULL;
		return;
	}

	size_t new_size = sizeof(struct u_metrics_sink) * g_metrics_sink_count;
	struct u_metrics_sink *new_sinks = (struct u_metrics_sink *)realloc(g_metrics_sinks, new_size);
	if (new_sinks) {
		g_metrics_sinks = new_sinks;
	}
}

static xrt_result_t
add_sink(struct u_metrics_sink definition)
{
	bool succeeded = false;

	os_mutex_lock(&g_metrics_mutex);

	size_t new_size = sizeof(struct u_metrics_sink) * (g_metrics_sink_count + 1);
	struct u_metrics_sink *new_sinks = (struct u_metrics_sink *)realloc(g_metrics_sinks, new_size);

	if (new_sinks) {
		g_metrics_sinks = new_sinks;

		struct u_metrics_sink *sink = &new_sinks[g_metrics_sink_count];
		*sink = definition;

		succeeded = write_version_to_sink_locked(VERSION_MAJOR, VERSION_MINOR, sink);
		if (succeeded) {
			g_metrics_sink_count++;
		}
	}

	os_mutex_unlock(&g_metrics_mutex);

	if (new_sinks == NULL) {
		return XRT_ERROR_OUT_OF_MEMORY;
	}

	if (!succeeded) {
		close_sink_stream_locked(&definition);
	}
	return XRT_SUCCESS;
}

static xrt_result_t
add_file(FILE *file, xrt_file_handle_t handle, bool early_flush)
{
	bool close_file = file != NULL;
	bool close_handle = !close_file && handle != XRT_FILE_HANDLE_INVALID;
	enum u_metrics_sink_type type;
	xrt_result_t result;

#if defined(XRT_OS_WINDOWS)

	if (file != NULL && handle == XRT_FILE_HANDLE_INVALID) {
		handle = _get_osfhandle(_fileno(file));
	}

	DWORD handle_type = FILE_TYPE_UNKNOWN;
	if (handle != XRT_FILE_HANDLE_INVALID) {
		SetLastError(0);
		handle_type = GetFileType(handle);
		if (handle_type == FILE_TYPE_UNKNOWN && GetLastError() != 0) {
			U_LOG_E("Could not get type of handle %p!", handle);
			result = XRT_ERROR_INVALID_ARGUMENT;
			goto done;
		}
	}

	switch (handle_type) {
	case FILE_TYPE_DISK:
	case FILE_TYPE_PIPE:
	case FILE_TYPE_CHAR:
	case FILE_TYPE_REMOTE:
	case FILE_TYPE_UNKNOWN: type = U_METRICS_SINK_TYPE_REGULAR_FILE; break;
	default: result = XRT_ERROR_INVALID_ARGUMENT; goto done;
	}

	if (handle_type == FILE_TYPE_PIPE) {
		DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
		if (!SetNamedPipeHandleState(handle, &mode, NULL, NULL)) {
			U_LOG_E("Could not set pipe handle %p to nonblocking!", handle);
			result = XRT_ERROR_INVALID_ARGUMENT;
			goto done;
		}
	}

	if (file == NULL) {
		int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY);
		if (fd == -1) {
			U_LOG_E("Could not convert handle %p to fd!", handle);
			result = XRT_ERROR_OUT_OF_MEMORY;
			goto done;
		}

		file = _fdopen(fd, "wb");
		if (file == NULL) {
			U_LOG_E("Could not open fd %d as file!", fd);
			_close(fd);
			result = XRT_ERROR_OUT_OF_MEMORY;
			goto done;
		}

		close_file = true;
		close_handle = false;
	}

#elif defined(XRT_OS_LINUX)

	if (file != NULL && handle == XRT_FILE_HANDLE_INVALID) {
		handle = fileno(file);
	}

	struct stat st;
	memset(&st, 0, sizeof(st));
	if (handle == XRT_FILE_HANDLE_INVALID) {
		st.st_mode = S_IFIFO;
	} else if (fstat(handle, &st) != 0) {
		result = XRT_ERROR_INVALID_ARGUMENT;
		goto done;
	}

	switch (st.st_mode & S_IFMT) {
	case S_IFREG:
	case S_IFBLK:
	case S_IFCHR: type = U_METRICS_SINK_TYPE_REGULAR_FILE; break;
	case S_IFSOCK: type = U_METRICS_SINK_TYPE_SOCKET; break;
	case S_IFDIR:
	case S_IFLNK:
	default: result = XRT_ERROR_INVALID_ARGUMENT; goto done;
	}

	if (type == U_METRICS_SINK_TYPE_REGULAR_FILE && file == NULL) {
		file = fdopen(handle, "wb");
		if (file == NULL) {
			U_LOG_E("Could not open fd %d as file!", handle);
			result = XRT_ERROR_OUT_OF_MEMORY;
			goto done;
		}

		close_file = true;
		close_handle = false;
	}

	if (type != U_METRICS_SINK_TYPE_REGULAR_FILE) {
		if (file != NULL) {
			int new_handle = dup(handle);
			if (new_handle == -1) {
				U_LOG_E("Could not duplicate fd %d!", handle);
				result = XRT_ERROR_OUT_OF_MEMORY;
				goto done;
			}

			handle = new_handle;
			close_handle = true;
		}

		int flags = fcntl(handle, F_GETFL, 0);
		if (flags == -1) {
			U_LOG_E("Could not get flags of file descriptor %d!", handle);
			result = XRT_ERROR_INVALID_ARGUMENT;
			goto done;
		}

		flags |= O_NONBLOCK;

		if (fcntl(handle, F_SETFL, flags) == -1) {
			U_LOG_E("Could not set file descriptor %d to nonblocking!", handle);
			result = XRT_ERROR_INVALID_ARGUMENT;
			goto done;
		}
	}

#else

	result = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	goto done;

#endif

	if (type == U_METRICS_SINK_TYPE_REGULAR_FILE && early_flush) {
		setvbuf(file, NULL, _IONBF, 0);
	}

	struct u_metrics_sink definition = {
	    .type = type,
	};
	switch (type) {
	case U_METRICS_SINK_TYPE_REGULAR_FILE: definition.file = file; break;
#if defined(XRT_OS_LINUX)
	case U_METRICS_SINK_TYPE_SOCKET: definition.socket = handle; break;
#endif
	default: assert(0 && "unhandled enum value");
	}

	result = add_sink(definition);
	if (result == XRT_SUCCESS) {
		if (type == U_METRICS_SINK_TYPE_REGULAR_FILE) {
			close_file = false;
		} else {
			close_handle = false;
		}
	}

done:
	if (close_file) {
		fclose(file);
	}
	if (close_handle) {
		xrt_file_handle_close(handle);
	}
	return result;
}

static void
write_record(monado_metrics_Record *r)
{
	uint8_t buffer[BUFFER_SIZE];
	size_t size = 0;

	if (g_metrics_sink_count < 1) {
		return;
	}

	encode_record(r, buffer, &size);

	if (size < 1) {
		return;
	}

	os_mutex_lock(&g_metrics_mutex);

	for (size_t i = 0; i < g_metrics_sink_count; i++) {
		struct u_metrics_sink *sink = &g_metrics_sinks[i];

		if (!sink->file || !write_buffer_to_sink_locked(buffer, size, sink)) {
			close_sink_locked(sink);
			i--;
		}
	}

	os_mutex_unlock(&g_metrics_mutex);
}


/*
 *
 * 'Exported' functions.
 *
 */

void
u_metrics_init(void)
{
	os_mutex_init(&g_metrics_mutex);

	g_metrics_initialized = true;

	const char *str = debug_get_option_metrics_file();
	if (str == NULL) {
		U_LOG_D("No metrics file!");
		return;
	}

	FILE *file = fopen(str, "wb");
	if (file == NULL) {
		U_LOG_E("Could not open '%s'!", str);
		return;
	}
	bool early_flush = debug_get_bool_option_metrics_early_flush();
	u_metrics_add_file(file, early_flush);

	U_LOG_I("Opened metrics file: '%s'", str); // NOLINT(clang-analyzer-unix.Stream)
}

xrt_result_t
u_metrics_add_file(FILE *file, bool early_flush)
{
	if (!g_metrics_initialized) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	return add_file(file, XRT_FILE_HANDLE_INVALID, early_flush);
}

xrt_result_t
u_metrics_add_file_handle(xrt_file_handle_t handle, bool early_flush)
{
	if (!g_metrics_initialized) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	return add_file(NULL, handle, early_flush);
}

void
u_metrics_close(void)
{
	if (!g_metrics_initialized) {
		return;
	}

	const char *str = debug_get_option_metrics_file();
	if (str != NULL) {
		U_LOG_I("Closing metrics file: '%s'", str);
	}

	// At least try to avoid races.
	os_mutex_lock(&g_metrics_mutex);

	for (size_t i = 0; i < g_metrics_sink_count; i++) {
		struct u_metrics_sink *sink = &g_metrics_sinks[i];

		close_sink_locked(sink);
	}

	free(g_metrics_sinks);
	g_metrics_sinks = NULL;

	os_mutex_unlock(&g_metrics_mutex);

	g_metrics_initialized = false;

	os_mutex_destroy(&g_metrics_mutex);
}

bool
u_metrics_is_active(void)
{
	return g_metrics_initialized && g_metrics_sink_count > 0;
}

void
u_metrics_write_session_frame(struct u_metrics_session_frame *umsf)
{
	if (!g_metrics_initialized) {
		return;
	}

	monado_metrics_Record record = monado_metrics_Record_init_default;

	// Select which filed is used.
	record.which_record = monado_metrics_Record_session_frame_tag;

#define COPY(_0, _1, _2, _3, FIELD, _4) (record.record.session_frame.FIELD = umsf->FIELD);
	monado_metrics_SessionFrame_FIELDLIST(COPY, 0);
#undef COPY


	write_record(&record);
}

void
u_metrics_write_used(struct u_metrics_used *umu)
{
	if (!g_metrics_initialized) {
		return;
	}

	monado_metrics_Record record = monado_metrics_Record_init_default;

	// Select which filed is used.
	record.which_record = monado_metrics_Record_used_tag;

#define COPY(_0, _1, _2, _3, FIELD, _4) (record.record.used.FIELD = umu->FIELD);
	monado_metrics_Used_FIELDLIST(COPY, 0);
#undef COPY


	write_record(&record);
}

void
u_metrics_write_system_frame(struct u_metrics_system_frame *umsf)
{
	if (!g_metrics_initialized) {
		return;
	}

	monado_metrics_Record record = monado_metrics_Record_init_default;

	// Select which filed is used.
	record.which_record = monado_metrics_Record_system_frame_tag;

#define COPY(_0, _1, _2, _3, FIELD, _4) (record.record.system_frame.FIELD = umsf->FIELD);
	monado_metrics_SystemFrame_FIELDLIST(COPY, 0);
#undef COPY


	write_record(&record);
}

void
u_metrics_write_system_gpu_info(struct u_metrics_system_gpu_info *umgi)
{
	if (!g_metrics_initialized) {
		return;
	}

	monado_metrics_Record record = monado_metrics_Record_init_default;

	// Select which filed is used.
	record.which_record = monado_metrics_Record_system_gpu_info_tag;

#define COPY(_0, _1, _2, _3, FIELD, _4) (record.record.system_gpu_info.FIELD = umgi->FIELD);
	monado_metrics_SystemGpuInfo_FIELDLIST(COPY, 0);
#undef COPY


	write_record(&record);
}

void
u_metrics_write_system_present_info(struct u_metrics_system_present_info *umpi)
{
	if (!g_metrics_initialized) {
		return;
	}

	monado_metrics_Record record = monado_metrics_Record_init_default;

	// Select which filed is used.
	record.which_record = monado_metrics_Record_system_present_info_tag;

#define COPY(_0, _1, _2, _3, FIELD, _4) (record.record.system_present_info.FIELD = umpi->FIELD);
	monado_metrics_SystemPresentInfo_FIELDLIST(COPY, 0);
#undef COPY


	write_record(&record);
}
