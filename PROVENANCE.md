# Provenance

This repository began as a verbatim extraction of the C Core Foundation implementation from Apple's
swift-corelibs-foundation, and carries the small changes Agora has made since.

| | |
|---|---|
| Upstream repository | https://github.com/swiftlang/swift-corelibs-foundation |
| Upstream revision | `swift-6.4.0-RELEASE` |
| Upstream commit | `d29d01ba165f6957141e07ea7fe8144ab491bc24` |
| Upstream commit date | 2026-08-18 |
| Extracted subtree | `Sources/CoreFoundation/` |
| Extraction date | 2026-10-10 |
| Source changes by Agora | Swift toolchain integration removed; standalone build added — see *Agora changes* |

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

### Swift toolchain integration removed

Agora does not support Swift, so the Swift toolchain integration has been removed:

- **deleted** `include/module.modulemap` and `include/static-module.map` — the Clang module maps that
  exposed the library to Swift;
- in **`CMakeLists.txt`**, removed the `INTERFACE` `-Xcc -fmodule-map-file=…` compile option and
  pointed the install rules at ordinary `include/CoreFoundation` and `lib` destinations, replacing
  upstream's `lib/swift` and `lib/swift_static` Swift-toolchain paths;
- also in **`CMakeLists.txt`**, removed the `Foundation_EXPORTS` global property (the Swift build's
  exported-targets hook) and the `file(COPY … _CModulesForClients …)` header copy that fed the Swift
  XCTest clients.

The C sources and headers are otherwise untouched. Upstream's Swift *runtime* surface is **kept**: the
`_CFSwiftBridge` / `_NS*Bridge` machinery, the `CF_SWIFT_*` attributes, and the `DEPLOYMENT_RUNTIME_SWIFT`
conditionals are C code, and `DEPLOYMENT_RUNTIME_SWIFT` is defined to `1` by default, so those paths
are the active Linux/BSD build.

### Standalone build

Upstream Core Foundation is a subdirectory of Apple's swift-corelibs-foundation *Swift* build, which
supplies the `project()`, the `_Foundation_common_build_flags` variable, the `_FoundationICU` target
and the `dispatch` dependency. `CMakeLists.txt` is now Agora's own top-level build for the library:

- **system ICU** — the `_foundation_unicode/` include prefix that the `swift-foundation-icu` package
  provides is recreated as a symbolic link to the system ICU headers, and `_FoundationICU` is an
  interface target over `ICU::uc`, `ICU::i18n` and `ICU::data`;
- **the `libagora-dispatch` sibling** — dispatch and the blocks runtime come from that component
  (built in-tree, or found with `find_package(dispatch CONFIG)`); an include shim exposes its public
  and private headers as `dispatch/*.h`, plus its `os/*.h`, which is the layout Core Foundation's
  sources expect;
- **`_Foundation_common_build_flags`** — defined for the C sources, with only the Swift-specific
  entries dropped.

Verified to compile cleanly with clang into `libCoreFoundation.a`.

## Licence and attribution

The extracted code is Apache-2.0 with the Runtime Library Exception. `LICENSE` is retained verbatim
and the per-file Apache-2.0 headers are untouched. Agora's own additions to this repository fall
under the project licence (see the top-level project README).

## Keeping in sync

Updates re-extract from a newer upstream revision and record it in the table above. Because the tree
sits at the repository root, `git diff` against a fresh upstream checkout shows local divergence
directly.
