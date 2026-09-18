#include "luau_native.h"
#include <cstdlib>

#if defined(_WIN32)
#define NATIVE_EXPORT __declspec(dllexport)
#else
#define NATIVE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" NATIVE_EXPORT void luau_native_free(void* buffer)
{
    std::free(buffer);
}
