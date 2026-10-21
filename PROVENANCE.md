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

### `CFBundle_Resources.c`: the executables subdirectory name

`_CFBundleGetPlatformExecutablesSubdirectoryName()` answered the **HOST's** name — `Linux` on Linux, `FreeBSD`
on the BSDs — so `CFBundleCopyExecutableURL` searched `Contents/Linux` and answered NULL for every modern
`.app`. Agora's bundles use **Apple's layout**, so a `.app`'s executable is in `Contents/MacOS` on every platform
Agora targets, and that function now answers `MacOS` there.

MEASURED, and it is how the line was found: before the change `CFBundleCopyExecutableURL` returned NULL for the
real `Workspace.app`; after it, `…/Workspace.app/Contents/MacOS/Workspace`. `_CFBundleGetCurrentPlatform()`, which
names the OS rather than a directory, is deliberately left at the host's answer. The alternative — resolving the
executable in the consumer (`libagora-launchservices`) — was built first and then removed in favour of fixing it
at the source.

### `CFBundle_Resources.c` / `CFBundle_Executable.c`: the platform and architecture axes

**A BUNDLE MAY CARRY ONE EXECUTABLE PER PLATFORM AND PER ARCHITECTURE, AND THE DIRECTORY — NOT THE FILE NAME —
CARRIES THE AXIS.** `Contents/Linux/amd64/Workspace`, `Contents/FreeBSD/amd64/Workspace`,
`Contents/NetBSD/aarch64/Workspace`; `Contents/MacOS/Workspace` remains the single-platform and legacy slot. The
reason the architecture is a directory rather than a suffix is the kernel: `comm` is the executable's basename
truncated to 15 characters on Linux, and `-[NSProcessInfo processName]` (`libagora-base/NSProcessInfo.m`) reads
it, so `Workspace-aarch64` would have titled the application menu `Workspace-aarch6`. A suffixed file name is
nonetheless ACCEPTED (`Contents/Linux/Workspace-amd64`), because refusing a layout that works costs more than an
extra `stat`, and the main-bundle probe below covers it.

`_CFBundleCopyExecutableURLInDirectory2()` now tries, in order: `<platform>/<arch>/<exe>`, `<platform>/<exe>-<arch>`,
`<platform>/<exe>`, then the flat `Contents/<exe>` and the old-style wrapper root. The platform list is the host's
name first (`Linux`, `FreeBSD`, `NetBSD`, `OpenBSD`, `MacOS`) with **`MacOS` last on Linux and the BSDs** as the
universal slot, so every bundle written before this change still resolves; the architecture list is the running
ISA with the other common spelling behind it (`amd64`/`x86_64`, `arm64`/`aarch64`, `riscv64`/`riscv`), and is
empty for an ISA this file does not know, which degenerates to the old search exactly. The FHS branch
(`libexec/<Name>.executables/`) goes through the same list.

**`CFBundleGetMainBundle()` WAS THE TRAP.** `_CFBundleCopyBundleURLForExecutablePath()` walked a path back to its
wrapper by comparing the last directory against `_CFBundleGetPlatformExecutablesSubdirectoryName()` alone, so
`…/Contents/Linux/amd64/Workspace` stopped at `…/Contents/Linux/amd64` — a directory with no `Contents/Info.plist`
— and the main bundle degraded to `NotABundle`, losing its identifier and its resources. It now strips an
architecture directory first, and only when the directory above it really is a platform directory (a bundle whose
own name is `arm64` is not a slice).

MEASURED against synthetic bundles: `Contents/Linux/amd64/Workspace` resolves; a legacy `Contents/MacOS/Workspace`
still resolves (the real `workspace/build/Workspace.app` included); a foreign-only slice
(`Contents/FreeBSD/amd64/Workspace`) and a wrong-architecture one (`Contents/Linux/arm64/Workspace`) answer NULL
rather than a slice this machine cannot run; and a binary run from inside a slice sees `CFBundleGetMainBundle()`
answer the `.app` wrapper with its `CFBundleIdentifier` intact, for both the directory and the suffixed-name
layouts.

KNOWN GAP, stated rather than implied: the reverse mapping still assumes the executable is at
`<Contents>/<platform>[/<arch>]/<exe>`. A framework nested deeper (`Contents/Frameworks/Linux/amd64/Foo.framework/…`)
walks back only to `…/Contents`, which is the same class of approximation upstream shipped and not a regression.

### `CFHyphenation.c`, which upstream does not have here

`CFString.h` declares `CFStringGetHyphenationLocationBeforeIndex` and
`CFStringIsHyphenationAvailableForLocale`, and nothing in this repository defined them — Apple's
implementation lives outside the source set this tree was extracted from. **Agora's `CFHyphenation.c` is
a new file rather than a change to an upstream one**, and it is the ONLY file in this component whose
content is not traced to the extraction above. It implements Liang's algorithm over the hyphenation
dictionaries a Linux system carries, which are read from `/usr/share/hyphen` at RUNTIME and deliberately
not vendored: `hyphen-en-us`'s Debian copyright states the patterns are `GPL-2+ or LGPL-2.1+ or
MPL-1.1+`, which cannot be copied into this MIT-licensed tree. Apple's own header makes the absent case
part of the contract — "Hyphenation data is not available for all locales" — so a locale with no
dictionary on the machine answers FALSE rather than guessing. The file's own header records the
algorithm, the measurements, and the one dictionary family whose intent this tier could not settle.

## Licence and attribution

The extracted code is Apache-2.0 with the Runtime Library Exception. `LICENSE` is retained verbatim
and the per-file Apache-2.0 headers are untouched. Agora's own additions to this repository fall
under the project licence (see the top-level project README).

## Keeping in sync

Updates re-extract from a newer upstream revision and record it in the table above. Because the tree
sits at the repository root, `git diff` against a fresh upstream checkout shows local divergence
directly.

## NORMALIZATION: Unicode's own test suite, and what it found

`libagora-base/tests/normalization.c` is the first suite in this tree that asks `CFStringNormalize` anything, and it
is a CONFORMANCE run before it is an example set.

- **THE DATA IS `NormalizationTest.txt`** (Unicode 15.1.0, 2.6 MB, 19,074 cases), each case five columns — source,
  NFC, NFD, NFKC, NFKD — with SIX invariants that every conformant implementation must satisfy. **IT IS NOT IN THIS
  REPOSITORY**, the same standing every other UCD file has: what is committed is `libagora-base/tools/gen-normalization-
  cases.py` (the recipe, with the URL) and the nine-case curated table it produces — the shortest case the data has for
  each construct the normalizer has a separate code path for.
- **THE SCORE IS 19,074 OF 19,074** — every case, every invariant. The committed table runs on every build (189
  checks); pointing `NORM_TEST` at the download runs the whole file, and the case ASSERTS the count, so a regression
  and an unrecorded improvement both fail.
- **AND IT FOUND ONE RULE MISSING, which is the argument for conformance data in one line.** Hangul composition
  (UAX #15 §3.12) is ARITHMETIC, not table lookup: L + V makes an LV syllable, LV + T makes an LVT one. Only the first
  half was implemented, so a leading jamo, an LV syllable and a trailing jamo normalized to itself instead of composing
  the final consonant — the trailing jamo fell through to the canonical table, which has no Hangul entries, got
  `0xFFFD` back and stayed a separate character. TWO OF THE 19,074 CASES, both that rule. Nothing in the tree had ever
  written that sequence.
