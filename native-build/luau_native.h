#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Free the buffer returned by luau_compile in the same native library that
// allocated it. Never use Marshal.FreeHGlobal or a different CRT's free().
void luau_native_free(void* buffer);

#ifdef __cplusplus
}
#endif
