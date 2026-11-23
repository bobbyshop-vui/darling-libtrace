/*
 * Copyright (c) 2019 PureDarwin Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <xpc/xpc.h>
#include "os_log_s.h"
#include "libtrace_assert.h"
#include <asl.h>
#include <os/log.h>
#import <os/object_private.h>

#ifdef os_log_create
#undef os_log_create
#endif

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

OS_OBJECT_OBJC_CLASS_DECL(os_log);

struct os_log_s _os_log_disabled = {
	.isa = NULL,
	.ref_cnt = _OS_OBJECT_GLOBAL_REFCNT,
	.xref_cnt = _OS_OBJECT_GLOBAL_REFCNT,
	.magic = OS_LOG_DISABLED_MAGIC,
	.subsystem = "",
	.category = ""
};

struct os_log_s _os_log_default = {
	.isa = NULL,
	.ref_cnt = _OS_OBJECT_GLOBAL_REFCNT,
	.xref_cnt = _OS_OBJECT_GLOBAL_REFCNT,
	.magic = OS_LOG_DEFAULT_MAGIC,
	.subsystem = "",
	.category = ""
};

static asl_object_t get_client() {
	static asl_object_t client = NULL;
	static dispatch_once_t token;
	dispatch_once(&token, ^{
		client = asl_open(NULL, "org.puredarwin.os_log", 0);
	});
	return client;
};

os_log_t os_log_create(const char *subsystem, const char *category) {
	libtrace_precondition(subsystem != NULL, "subsystem cannot be NULL");
	libtrace_precondition(category != NULL, "category cannot be NULL");

	os_log_t value = (os_log_t)_os_object_alloc(&OS_OBJECT_CLASS_SYMBOL(os_log), sizeof(struct os_log_s));
	value->magic = OS_LOG_MAGIC;
	value->subsystem = strdup(subsystem);
	value->category = strdup(category);
	return value;
}

bool os_log_type_enabled(os_log_t log, os_log_type_t type) {
	if (log == NULL) return false;
	if (log->magic == OS_LOG_DISABLED_MAGIC) return false;

	return true;
}

__XNU_PRIVATE_EXTERN
char *os_log_decode_buffer(const char *formatString, uint8_t *buffer, uint32_t bufferSize);
__XNU_PRIVATE_EXTERN
const char *os_log_buffer_to_hex_string(const uint8_t *buffer, uint32_t buffer_size);

void
_os_log_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size) {
	libtrace_precondition(log != NULL, "os_log_t cannot be NULL");
	if (log->magic == OS_LOG_DISABLED_MAGIC) return;
	libtrace_precondition(log->magic == OS_LOG_DEFAULT_MAGIC || log->magic == OS_LOG_MAGIC, "Invalid os_log_t pointer parameter passed to _os_log_impl()");
	libtrace_precondition(type >= OS_LOG_TYPE_DEFAULT && type <= OS_LOG_TYPE_FAULT, "Invalid os_log_type_t parameter passed to _os_log_impl()");

	aslmsg message = asl_new(ASL_TYPE_MSG);
	asl_set(message, "os_log(3)", "TRUE");

	const char *subsystem = log->subsystem;
	if (strlen(subsystem) == 0) subsystem = "(default)";
	const char *category = log->category;
	if (strlen(category) == 0) category = "(default)";

	asl_set(message, "Subsystem", subsystem);
	asl_set(message, "Category", category);

	const char *buffer_hex = os_log_buffer_to_hex_string(buf, size);
	asl_set(message, "HexBuffer", buffer_hex);

	int level;
	switch (type) {
		case OS_LOG_TYPE_DEBUG:
			level = ASL_LEVEL_DEBUG;
			break;

		case OS_LOG_TYPE_INFO:
		case OS_LOG_TYPE_DEFAULT:
			level = ASL_LEVEL_INFO;
			break;

		case OS_LOG_TYPE_ERROR:
			level = ASL_LEVEL_ERR;
			break;

		case OS_LOG_TYPE_FAULT:
			level = ASL_LEVEL_ALERT;
			break;

		default:
			libtrace_assert(false, "Invalid os_log_type_t not caught by precondition");
	}

	char *decodedBuffer = os_log_decode_buffer(format, buf, size);
	asl_log(get_client(), message, level, "%s", decodedBuffer);
	asl_release(message);
	free(decodedBuffer);
	free((void *)buffer_hex);
}

/*
 * Xcode (and other Apple binaries built against the 10.12+ SDK) expand
 * os_log_error()/os_log_fault() into calls to these two private entry points
 * rather than into _os_log_impl().  They were never added here, so dyld failed
 * the lazy bind with
 *
 *     Symbol not found: __os_log_error_impl
 *     Expected in: /usr/lib/libSystem.B.dylib
 *
 * and the process aborted.  Darwin declares them as
 *
 *     void _os_log_error_impl(void *dso, os_log_t log, os_log_type_t type,
 *                             const void *buf, uint32_t size, uint16_t flags);
 *
 * with _os_log_fault_impl() identical.  We deliberately log nothing: the
 * message has already been packed into `buf` by the caller, and decoding it is
 * not worth the risk during startup.  The parameters are ignored, so the exact
 * arity does not matter for ABI compatibility.
 */
void
_os_log_error_impl(void *dso __unused, os_log_t log __unused, os_log_type_t type __unused,
		const void *buf __unused, uint32_t size __unused, uint16_t flags __unused) {
}

void
_os_log_fault_impl(void *dso __unused, os_log_t log __unused, os_log_type_t type __unused,
		const void *buf __unused, uint32_t size __unused, uint16_t flags __unused) {
}

#pragma mark Legacy Functions

os_log_t _os_log_create(void *dso __unused, const char *subsystem, const char *category) {
	return os_log_create(subsystem, category);
}

bool os_log_is_enabled(os_log_t log) {
	return true;
}

bool os_log_is_debug_enabled(os_log_t log) {
	return os_log_type_enabled(log, OS_LOG_TYPE_DEBUG);
}
