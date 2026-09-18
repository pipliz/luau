#pragma once
#include "luau_native.h"
#include <stdint.h>

// ABI 2. All buffers are length-prefixed little-endian values; no Lua stack is
// exposed to managed callbacks. Returned buffers belong to the VM until next call.
typedef struct luau_host_vm luau_host_vm;
typedef int (*luau_host_callback)(void* user, int operation, const uint8_t* input,
    int input_size, const uint8_t** output, int* output_size);
// Callback output is borrowed until the next callback or until host_call returns.
// Return an error category (zero on success); never throw across this boundary.
enum luau_host_error_kind { LUAU_OK=0, LUAU_SCRIPT_ERROR=1, LUAU_TIMEOUT=2,
    LUAU_MEMORY_LIMIT=3, LUAU_INVALID_HANDLE=4, LUAU_BAD_ARGUMENT=5, LUAU_HOST_ERROR=6,
    LUAU_RESOURCE_LIMIT=7 };
typedef struct luau_host_options {
    uint32_t size, abi;
    uint64_t memory_limit;
    uint32_t payload_limit, reference_limit, host_call_limit, handle_limit;
} luau_host_options;
typedef struct luau_host_stats {
    uint32_t size, live_references;
    uint64_t memory_used, memory_peak, host_calls;
    uint32_t last_host_calls, live_handles;
} luau_host_stats;
#ifdef __cplusplus
extern "C" {
#endif
LUAU_NATIVE_API int luau_host_abi(void);
LUAU_NATIVE_API luau_host_vm* luau_host_create(const luau_host_options* options, luau_host_callback callback, void* user);
LUAU_NATIVE_API void luau_host_destroy(luau_host_vm* vm);
LUAU_NATIVE_API int luau_host_bind(luau_host_vm* vm, const char* name, int operation);
LUAU_NATIVE_API int luau_host_module(luau_host_vm* vm, const char* name, const char* source, int length);
LUAU_NATIVE_API int luau_host_call(luau_host_vm* vm, const char* module, const char* member,
    int reference, const uint8_t* arguments, int length, int milliseconds,
    const uint8_t** result, int* result_size);
LUAU_NATIVE_API int luau_host_error_kind(luau_host_vm* vm);
LUAU_NATIVE_API const char* luau_host_traceback(luau_host_vm* vm);
LUAU_NATIVE_API int luau_host_release(luau_host_vm* vm, int reference);
LUAU_NATIVE_API int luau_host_statistics(luau_host_vm* vm, luau_host_stats* stats);
LUAU_NATIVE_API int luau_host_constant(luau_host_vm* vm, const char* name, const uint8_t* value, int length);
LUAU_NATIVE_API const char* luau_host_error(luau_host_vm* vm);
#ifdef __cplusplus
}
#endif
