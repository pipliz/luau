# Luau native binaries

This fork builds native shared libraries for Colony Survival. C# bindings and
Unity integration belong in the Colony monorepo, not here. Upstream runtime and
compiler sources are unchanged. The build is pinned to the upstream commit in
`version.json`; the fork's default branch is `codex/native-binaries`.

## Contents

Each platform gets **one shared library** containing the Luau interpreter and
source-to-bytecode compiler. There is no JIT/native code generator, type checker,
CLI or filesystem-based `require` implementation in this library. The compiler
turns `.luau` source into bytecode for the interpreter; it does not produce native
machine code. The host must implement module loading if it needs `require`.

| Runtime identifier | Library | Build/test host |
| --- | --- | --- |
| win-x64 | `lib/luau.dll` | Windows Server 2022, x64 |
| linux-x64 | `lib/libluau.so` | Ubuntu 22.04, x64 |
| osx-x64 | `lib/libluau.dylib` | macOS 15, Intel |
| osx-arm64 | `lib/libluau.dylib` | macOS 15, Apple Silicon |

The macOS deployment target is 13.0; execution is tested on the listed CI host,
not every older OS. Linux uses the system libc/libstdc++ from the Ubuntu 22.04
build environment. Windows links its MSVC runtime statically. These are separate
Mac architecture packages, not a universal binary. Mobile, WebGL and consoles
are outside the current build matrix.

## Download and verify

Use the fork's **Releases** for versioned archives. **Actions > Native libraries**
also retains each successful build's artifacts for 14 days. Archives contain the
library, public C headers, licenses, `manifest.json` and `SHA256SUMS`. A separate
`.zip.sha256` file covers the complete archive. The manifest identifies the
upstream commit, fork commit, compiler, build configuration, ABI settings and CI
run. Checksums detect changes; they do not by themselves establish authenticity.
Obtain them from the same trusted GitHub release/build and inspect its source.

## ABI contract for future bindings

- Public VM/compiler entry points use C linkage and the normal C calling
  convention (`CallingConvention.Cdecl` for future P/Invoke declarations).
- Three-component float vectors: `LUA_VECTOR_SIZE=3`, `LUA_VECTOR_DOUBLE=0`.
- `LUA_USE_LONGJMP=1`; use protected calls. A future binding must contain native
  error handling around managed callbacks; a longjmp must not cross managed frames.
- Release build; no CPU-specific `-march=native` tuning.
- Treat `lua_State` as opaque. Match declarations to the headers shipped in the
  exact binary release; do not assume ABI or bytecode stability across upgrades.
- Free the pointer returned by `luau_compile` with **`luau_native_free`**, the one
  small packaging helper added here. Do not use `Marshal.FreeHGlobal`, libc from
  another module or another CRT. Close each VM with `lua_close`.
- C header convenience macros are not exported functions. Bind their underlying
  functions (for example, `lua_tonumberx` rather than `lua_tonumber`).
- Native consumers should include the upstream C headers inside an `extern "C"`
  block when compiling as C++; the export definitions are build-side settings.

The Python/ctypes smoke test loads the actual shared library, compiles and runs
Luau source, invokes a host callback, checks syntax/runtime errors, and repeatedly
creates/destroys states. CI repeats it against the installed artifact. It is not
a Unity/IL2CPP compatibility test; those tests belong with the future bindings.

## Local build

Requires CMake 3.24+, a C++17 compiler, and Python 3 for ABI tests/packaging.
From the repository root:

```sh
cmake -S native-build -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
cmake --install build --config Release --prefix build/stage
python native-build/package.py --rid linux-x64
```

Use the matching RID when packaging another platform. On Windows, add `-A x64`
when configuring with Visual Studio. For Mac, also set
`-DCMAKE_OSX_ARCHITECTURES=x86_64` or `arm64` and
`-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0`, matching CI. CMake reuses upstream source
lists and compiler options but combines the selected components in a single
shared target; upstream's separate shared-library build is not used.

## Updating and releasing

1. Merge a reviewed upstream release into `codex/native-binaries`.
2. Update the upstream commit/version and package version in `version.json`.
3. Push and require all four **Native libraries** jobs to pass. Review exported
   APIs and adjust the Colony bindings whenever the native ABI changes.
4. Tag the tested commit `native-<package_version>`, for example
   `native-0.738-native.1`, and push the tag. The workflow validates the version,
   rebuilds/tests all targets, then publishes their archives in a GitHub release.

Build jobs have read-only repository permissions. Only the tag-triggered release
job can write releases. Official checkout/artifact actions are pinned to commit
SHAs. Standard GitHub-hosted runners are used. Inherited upstream workflows are
disabled in this fork; the native workflow is its only active pipeline.
