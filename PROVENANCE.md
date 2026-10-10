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
| Source changes by Agora | Swift toolchain integration removed — see *Agora changes* |

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

Agora does not support Swift, so the Swift toolchain integration has been removed:

- **deleted** `include/module.modulemap` and `include/static-module.map` — the Clang module maps that
  exposed the library to Swift;
- in **`CMakeLists.txt`**, removed the `INTERFACE` `-Xcc -fmodule-map-file=…` compile option and
  pointed the install rules at ordinary `include/CoreFoundation` and `lib` destinations, replacing
  upstream's `lib/swift` and `lib/swift_static` Swift-toolchain paths.

The C sources and headers are otherwise untouched. Upstream's Swift *runtime* surface is **kept**: the
`_CFSwiftBridge` / `_NS*Bridge` machinery, the `CF_SWIFT_*` attributes, and the `DEPLOYMENT_RUNTIME_SWIFT`
conditionals are C code, and `DEPLOYMENT_RUNTIME_SWIFT` is defined to `1` by default, so those paths
are the active Linux/BSD build.

## Licence and attribution

The extracted code is Apache-2.0 with the Runtime Library Exception. `LICENSE` is retained verbatim
and the per-file Apache-2.0 headers are untouched. Agora's own additions to this repository fall
under the project licence (see the top-level project README).

## Keeping in sync

Updates re-extract from a newer upstream revision and record it in the table above. Because the tree
sits at the repository root, `git diff` against a fresh upstream checkout shows local divergence
directly.
