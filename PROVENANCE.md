# Provenance

This repository began as a verbatim extraction of the C Core Foundation implementation from
Apple's swift-corelibs-foundation.

| | |
|---|---|
| Upstream repository | https://github.com/swiftlang/swift-corelibs-foundation |
| Upstream revision | `swift-6.4.0-RELEASE` |
| Upstream commit | `d29d01ba165f6957141e07ea7fe8144ab491bc24` |
| Upstream commit date | 2026-08-18 |
| Extracted subtree | `Sources/CoreFoundation/` |
| Extraction date | 2026-10-10 |
| Source changes by Agora | none |

## What was copied

The complete contents of upstream `Sources/CoreFoundation/`, placed at this repository's root:

- the C implementation — `CF*.c` and `uuid.c` (88 `.c` files);
- the public headers — `include/`;
- the internal headers — `internalInclude/`;
- the vendored blocks runtime — `BlockRuntime/`;
- `Tools/`, `Info.plist`, the symbol-alias files (`SymbolAliases`, `DarwinSymbolAliases`), the
  Unicode data tables, the module maps, and upstream's `CMakeLists.txt`.

119 `.h` files in total across `include/` and `internalInclude/`.

Also copied: upstream's `LICENSE` (Apache-2.0 with the Runtime Library Exception).

## Licence and attribution

The extracted code is Apache-2.0 with the Runtime Library Exception. `LICENSE` is retained verbatim
and the per-file Apache-2.0 headers are untouched. Agora's own additions to this repository fall
under the project licence (see the top-level project README).

## Keeping in sync

Updates re-extract from a newer upstream revision and record it in the table above. Because the tree
sits at the repository root, `git diff` against a fresh upstream checkout shows local divergence
directly.
