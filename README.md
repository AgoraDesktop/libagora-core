# libagora-core

**Core Foundation for Agora** — the C foundation layer of Agora's Cocoa-compatible stack:
`CFString`, `CFArray`, `CFDictionary`, `CFRunLoop`, `CFURL`, the property-list parser, and the
rest of the Core Foundation surface that Foundation is built on.

Agora is an installable desktop environment for Linux and BSD; see the project README in the parent
directory (`../README.md`). `libagora-core` is the bottom layer of its stack.


**AND NORMALIZATION IS NOW MEASURED AGAINST UNICODE'S OWN SUITE.** `CFStringNormalize` had existed here with nothing
ever asking it a conformance case; `libagora-base/tests/normalization.c` now runs the nine-case curated table from
`NormalizationTest.txt` on every build and the whole 19,074-case file whenever `NORM_TEST` names the download.
**19,074 of 19,074 satisfy all six invariants** — after the run found one rule missing: Hangul composition is
arithmetic, not table lookup, and only the L + V half was implemented, so `1100 AC00 11A8` failed to compose its
trailing jamo. See `PROVENANCE.md` for the record and `CFStringNormalize` for the note beside the fix.

## Provenance

The C sources here are extracted from Apple's
[swift-corelibs-foundation](https://github.com/swiftlang/swift-corelibs-foundation) — the
cross-platform Foundation used by Swift on Linux and Windows.

| | |
|---|---|
| Upstream | https://github.com/swiftlang/swift-corelibs-foundation |
| Revision | `swift-6.4.0-RELEASE` (`d29d01ba165f6957141e07ea7fe8144ab491bc24`, 2026-08-18) |
| Extracted subtree | `Sources/CoreFoundation/` |
| Agora changes | Swift toolchain integration removed; standalone build added — see [`PROVENANCE.md`](PROVENANCE.md) |

The full record is in [`PROVENANCE.md`](PROVENANCE.md).

## Licence

Upstream Core Foundation is **Apache-2.0 with the Runtime Library Exception** — permissive, and
compatible with Agora's permissive-only policy. The [`LICENSE`](LICENSE) file and the upstream
per-file headers are retained unchanged.

## Build

Build into `build/`, with **clang**, against the **system ICU** and the sibling
**`libagora-dispatch`** component — which is a separate repository and is built on its own:

```sh
# 1. build and install the sibling component first (into its own build/)
cmake -S ../libagora-dispatch -B ../libagora-dispatch/build \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build ../libagora-dispatch/build
cmake --install ../libagora-dispatch/build --prefix "$PWD/../build-prefix"

# 2. build Core Foundation against it
cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_PREFIX_PATH="$PWD/../build-prefix"
cmake --build build
```

- `libagora-dispatch` is found with `find_package(dispatch CONFIG)` from that prefix (override with
  `-Ddispatch_DIR=<dir>`).
- The system ICU is used directly (Agora neither vendors ICU nor pins `swift-foundation-icu`); the
  `_foundation_unicode/` include prefix is recreated as a link to the system ICU headers.
- The build is **Swift-free**: Core Foundation is compiled in its non-Swift (Objective-C)
  configuration, so nothing here links the Swift runtime (see [`PROVENANCE.md`](PROVENANCE.md)).
- It produces the static library `build/libagora-core.a`; a CFString/CFArray/CFURL/CFRunLoop
  smoke test links and runs. GCC is not a supported compiler.

## Consuming

The build installs a CMake package, so another component links it directly:

```cmake
find_package(CoreFoundation CONFIG REQUIRED)
target_link_libraries(app PRIVATE CoreFoundation::CoreFoundation)
```

Configure with `-DCMAKE_PREFIX_PATH=<install prefix>` and
`-Ddispatch_DIR=<libagora-dispatch>/build/cmake/modules`. That pulls in ICU, threads, `libm`/`libdl`
and the `dispatch` component, and adds the compile options Core Foundation's headers need
(`-fblocks`, `-fconstant-cfstrings`).

See [`PROVENANCE.md`](PROVENANCE.md) for how this differs from upstream's Swift-based build.
