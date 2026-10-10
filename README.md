# libagora-core

**Core Foundation for Agora** — the C foundation layer of Agora's Cocoa-compatible stack:
`CFString`, `CFArray`, `CFDictionary`, `CFRunLoop`, `CFURL`, the property-list parser, and the
rest of the Core Foundation surface that Foundation is built on.

Agora is an installable desktop environment for Linux and BSD; see the project README in the parent
directory (`../README.md`). `libagora-core` is the bottom layer of its stack.

## Provenance

The C sources here are extracted verbatim from Apple's
[swift-corelibs-foundation](https://github.com/swiftlang/swift-corelibs-foundation) — the
cross-platform Foundation used by Swift on Linux and Windows.

| | |
|---|---|
| Upstream | https://github.com/swiftlang/swift-corelibs-foundation |
| Revision | `swift-6.4.0-RELEASE` (`d29d01ba165f6957141e07ea7fe8144ab491bc24`, 2026-08-18) |
| Extracted subtree | `Sources/CoreFoundation/` |
| Agora changes | none yet — the subtree is copied verbatim to this repository's root |

The full record is in [`PROVENANCE.md`](PROVENANCE.md).

## Licence

Upstream Core Foundation is **Apache-2.0 with the Runtime Library Exception** — permissive, and
compatible with Agora's permissive-only policy. The [`LICENSE`](LICENSE) file and the upstream
per-file headers are retained unchanged.

## Build

**Not yet wired.** The upstream `CMakeLists.txt` kept at this root is entangled with the Swift
build — it expects a Swift compiler, the `swift-foundation-icu` package, and libdispatch. A
standalone build for Agora (against the system ICU and a vendored blocks runtime) is a later step.
