# Provenance

This repository began as a verbatim extraction of the C Core Foundation implementation from Apple's
swift-corelibs-foundation, and carries the changes Agora has made since.

| | |
|---|---|
| Upstream repository | https://github.com/swiftlang/swift-corelibs-foundation |
| Upstream revision | `swift-6.4.0-RELEASE` |
| Upstream commit | `d29d01ba165f6957141e07ea7fe8144ab491bc24` |
| Upstream commit date | 2026-08-18 |
| Extracted subtree | `Sources/CoreFoundation/` |
| Extraction date | 2026-10-10 |
| Source changes by Agora | Swift removed entirely (toolchain *and* runtime); standalone Swift-free build added — see *Agora changes* |

## What was copied

The complete contents of upstream `Sources/CoreFoundation/`, placed at this repository's root:

- the C implementation — `CF*.c` and `uuid.c` (88 `.c` files);
- the public headers — `include/`;
- the internal headers — `internalInclude/`;
- the vendored blocks runtime — `BlockRuntime/`;
- `Tools/`, `Info.plist`, the symbol-alias files (`SymbolAliases`, `DarwinSymbolAliases`), the
  Unicode data tables, the Swift module maps (`include/module.modulemap`, `include/static-module.map`),
  and upstream's `CMakeLists.txt`.

119 `.h` files in total across `include/` and `internalInclude/`.

Also copied: upstream's `LICENSE` (Apache-2.0 with the Runtime Library Exception).

## Agora changes

Agora does not support Swift, so nothing Swift remains — neither the Swift *toolchain* integration
nor the Swift *runtime* deployment that upstream targets.

### Swift toolchain integration removed

- **deleted** `include/module.modulemap` and `include/static-module.map` — the Clang module maps that
  exposed the library to Swift;
- in **`CMakeLists.txt`**, removed the `INTERFACE` `-Xcc -fmodule-map-file=…` compile option and
  pointed the install rules at ordinary `include/CoreFoundation` and `lib` destinations, replacing
  upstream's `lib/swift` and `lib/swift_static` Swift-toolchain paths;
- also in **`CMakeLists.txt`**, removed the `Foundation_EXPORTS` global property (the Swift build's
  exported-targets hook) and the `file(COPY … _CModulesForClients …)` header copy that fed the Swift
  XCTest clients.

### Swift runtime deployment disabled

Core Foundation as extracted *is* a Swift-runtime component: `DEPLOYMENT_RUNTIME_SWIFT` is defined to
`1` (in two headers), constant strings point at `_NSCFConstantString` type metadata, and
retain/release call `swift_retain` / `swift_release`. Built that way it cannot link without the Swift
runtime. Agora builds the **non-Swift (Objective-C) configuration** instead:

- **`DEPLOYMENT_RUNTIME_SWIFT` no longer defined** — removed from both `include/CFAvailability.h` and
  `internalInclude/CoreFoundation_Prefix.h`, and the `-DDEPLOYMENT_RUNTIME_SWIFT` compile flag
  dropped; `-fcf-runtime-abi=swift` replaced by `-fcf-runtime-abi=objc`;
- **`CFPlatform.c`** — the whole platform layer (thread helpers, `_CFEnviron`, `_CFOpenFile`,
  `_CFReallocf`, the `posix_spawn` machinery, lines 1683–2478) sat inside `#if DEPLOYMENT_RUNTIME_SWIFT`,
  i.e. upstream compiles it on Linux *only* in the Swift configuration. It is now compiled
  unconditionally, with its thread-specific-data retain/release converted from
  `swift_retain`/`swift_release` to `CFRetain`/`CFRelease`;
- **`CFPlatform.c`** — added `_CFGetCurrentDirectory`. Upstream declares it
  (`internalInclude/CFInternal.h`) but does not define it in this tree; in the Swift build the Swift
  Foundation layer supplies it;
- **`internalInclude/CFInternal.h`** — includes `ForSwiftFoundationOnly.h` unconditionally. Despite
  its name, that header holds thread / XDG / posix declarations the non-Swift path needs;
- **`CFRuntime.c`** — replaced the undefined `__kCFAllocatorTypeID_CONST` (referenced but defined
  nowhere in this tree) with `_kCFRuntimeIDCFAllocator`.

These are the only C source changes; the rest of the C sources and headers are untouched. Upstream's
Swift-facing *attributes* (`CF_SWIFT_NAME`, `swift_attr`, the `_CFSwiftBridge` bridges) remain — they
are C preprocessor macros that compile away, not a Swift dependency.

### Standalone build

Upstream Core Foundation is a subdirectory of Apple's swift-corelibs-foundation *Swift* build, which
supplies the `project()`, the `_Foundation_common_build_flags` variable, the `_FoundationICU` target
and the `dispatch` dependency. `CMakeLists.txt` is now Agora's own top-level build for the library:

- **system ICU** — the `_foundation_unicode/` include prefix that the `swift-foundation-icu` package
  provides is recreated as a symbolic link to the system ICU headers, and `_FoundationICU` is an
  interface target over `ICU::uc`, `ICU::i18n` and `ICU::data`;
- **the `libagora-dispatch` sibling** — dispatch and the blocks runtime are a separate component,
  built on its own into its own `build/` and found with `find_package(dispatch CONFIG)`; an include
  shim adds its private headers (which share the `dispatch/` prefix) and its `os/*.h`, the layout
  Core Foundation's sources expect;
- **`_Foundation_common_build_flags`** — defined for the C sources, with the Swift-specific entries
  dropped;
- **the artifact name** — the static library is built as `libagora-core.a`
  (`set_target_properties(CoreFoundation PROPERTIES OUTPUT_NAME agora-core)`); the CMake target keeps
  upstream's name, `CoreFoundation`;
- **a CMake package** — the build installs an export plus `CoreFoundationConfig.cmake`, so another
  component consumes it with `find_package(CoreFoundation CONFIG)`. The exported target carries the
  usage requirements a consumer needs: `-fblocks` and `-fconstant-cfstrings` (Core Foundation's
  headers use blocks and `CFSTR`), the `m` and `dl` libraries, and the `dispatch` / ICU / Threads
  dependencies, which the package config finds.

Verified: `libagora-core.a` builds with clang (0 warnings); a CFString / CFArray / CFURL / CFRunLoop
smoke test links and runs; and a standalone consumer project using `find_package(CoreFoundation CONFIG)`
against an installed prefix configures, links and runs.

## Licence and attribution

The extracted code is Apache-2.0 with the Runtime Library Exception. `LICENSE` is retained verbatim
and the per-file Apache-2.0 headers are untouched. Agora's own additions to this repository fall
under the project licence (see the top-level project README).

## Keeping in sync

Updates re-extract from a newer upstream revision and record it in the table above. Because the tree
sits at the repository root, `git diff` against a fresh upstream checkout shows local divergence
directly.
