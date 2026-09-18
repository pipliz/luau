#include "luau_native.h"
#include <cstdlib>

void luau_native_free(void* buffer)
{
    std::free(buffer);
}
