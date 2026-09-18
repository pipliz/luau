#pragma once
#include <stddef.h>

#if defined(_WIN32)
#if defined(LUAU_NATIVE_BUILD)
#define LUAU_NATIVE_API __declspec(dllexport)
#else
#define LUAU_NATIVE_API __declspec(dllimport)
#endif
#else
#define LUAU_NATIVE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Free the buffer returned by luau_compile in the same native library that
// allocated it. Never use Marshal.FreeHGlobal or a different CRT's free().
LUAU_NATIVE_API void luau_native_free(void* buffer);

#ifdef __cplusplus
}
#endif
