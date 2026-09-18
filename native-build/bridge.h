#pragma once
#include "luau_native.h"
#include <stdint.h>

// ABI 1. All buffers are length-prefixed little-endian values; no Lua stack is
// exposed to managed callbacks. Returned buffers belong to the VM until next call.
typedef struct luau_host_vm luau_host_vm;
typedef int (*luau_host_callback)(void* user, int operation, const uint8_t* input,
    int input_size, uint8_t* output, int output_capacity);
#ifdef __cplusplus
extern "C" {
#endif
LUAU_NATIVE_API int luau_host_abi(void);
LUAU_NATIVE_API luau_host_vm* luau_host_create(uint64_t memory_limit, luau_host_callback callback, void* user);
LUAU_NATIVE_API void luau_host_destroy(luau_host_vm* vm);
LUAU_NATIVE_API int luau_host_bind(luau_host_vm* vm, const char* name, int operation);
LUAU_NATIVE_API int luau_host_module(luau_host_vm* vm, const char* name, const char* source, int length);
LUAU_NATIVE_API int luau_host_call(luau_host_vm* vm, const char* module, const char* member,
    int reference, const uint8_t* arguments, int length, int milliseconds,
    const uint8_t** result, int* result_size);
LUAU_NATIVE_API const char* luau_host_error(luau_host_vm* vm);
#ifdef __cplusplus
}
#endif
