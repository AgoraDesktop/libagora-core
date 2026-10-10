#===----------------------------------------------------------------------===#
#
# Agora's standalone build for Core Foundation (libagora-core).
#
# Upstream Core Foundation is a subdirectory of Apple's swift-corelibs-foundation
# Swift build, which supplies the `project()`, the `_Foundation_common_build_flags`
# variable, the `_FoundationICU` target and the `dispatch` dependency. This file is
# Agora's replacement for that scaffolding, so libagora-core builds on its own with
# clang against:
#
#   * the *system* ICU (a `_foundation_unicode/` include prefix is recreated as a
#     link to the system ICU headers, in place of the `swift-foundation-icu`
#     package); and
#   * the sibling `libagora-dispatch` component — a separate repository, built on
#     its own and found here with `find_package(dispatch CONFIG)`.
#
# Build into `build/`:
#
#   cmake -S . -B build
#   cmake --build build
#
# See PROVENANCE.md for what Agora changed relative to upstream.
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.24)

project(AgoraCore LANGUAGES C)

include(GNUInstallDirs)
include(CheckSymbolExists)

set(CMAKE_POSITION_INDEPENDENT_CODE YES)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependency: the sibling libagora-dispatch component, built on its own into its
# own build/. Point `-Ddispatch_DIR=<libagora-dispatch>/build/cmake/modules` (or
# any directory holding a `dispatchConfig.cmake`) if the default guess misses.
# ---------------------------------------------------------------------------
set(AGORA_DISPATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libagora-dispatch"
    CACHE PATH "Source directory of the libagora-dispatch component")
if(NOT dispatch_DIR AND EXISTS "${AGORA_DISPATCH_DIR}/build/cmake/modules/dispatchConfig.cmake")
  set(dispatch_DIR "${AGORA_DISPATCH_DIR}/build/cmake/modules" CACHE PATH
      "Directory holding libagora-dispatch's dispatchConfig.cmake")
endif()
find_package(dispatch CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Include shim: the header layout Core Foundation's sources expect.
#   <dispatch/*.h>            libagora-dispatch public + private headers
#   <os/*.h>                  libagora-dispatch's os_object headers
#   <_foundation_unicode/*.h> the system ICU headers
# (The dispatch/ and os/ entries add the *private* headers, which share the
# `dispatch/` prefix, and the ICU prefix Core Foundation was written against.)
# ---------------------------------------------------------------------------
find_package(ICU REQUIRED COMPONENTS uc i18n data)

set(_agora_include_dir "${CMAKE_BINARY_DIR}/agora-include")
file(REMOVE_RECURSE "${_agora_include_dir}")
file(MAKE_DIRECTORY "${_agora_include_dir}/dispatch")
file(GLOB _agora_dispatch_headers
     "${AGORA_DISPATCH_DIR}/dispatch/*.h"
     "${AGORA_DISPATCH_DIR}/private/*.h")
foreach(_hdr IN LISTS _agora_dispatch_headers)
  get_filename_component(_name "${_hdr}" NAME)
  file(CREATE_LINK "${_hdr}" "${_agora_include_dir}/dispatch/${_name}" SYMBOLIC)
endforeach()
file(CREATE_LINK "${AGORA_DISPATCH_DIR}/os"
     "${_agora_include_dir}/os" SYMBOLIC)

set(_agora_icu_include_root "")
foreach(_dir IN LISTS ICU_INCLUDE_DIRS)
  if(EXISTS "${_dir}/unicode/ucal.h")
    set(_agora_icu_include_root "${_dir}")
  endif()
endforeach()
if(NOT _agora_icu_include_root)
  message(FATAL_ERROR "System ICU headers not found (looked for unicode/ucal.h).")
endif()
file(CREATE_LINK "${_agora_icu_include_root}/unicode"
     "${_agora_include_dir}/_foundation_unicode" SYMBOLIC)

# `_FoundationICU` — upstream's name for Core Foundation's ICU dependency,
# backed here by the system ICU.
add_library(_FoundationICU INTERFACE)
target_include_directories(_FoundationICU INTERFACE "${_agora_include_dir}")
target_link_libraries(_FoundationICU INTERFACE ICU::uc ICU::i18n ICU::data)

# ---------------------------------------------------------------------------
# Common build flags. Upstream sets these in the parent Swift build; the Swift
# specific entries are omitted. `-DDEPLOYMENT_RUNTIME_SWIFT` is kept: despite its
# name it selects the non-Darwin code path in Core Foundation (and is also
# defaulted to 1 in internalInclude/CoreFoundation_Prefix.h).
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

check_symbol_exists("strlcat" "string.h" HAVE_STRLCAT)
check_symbol_exists("strlcpy" "string.h" HAVE_STRLCPY)
check_symbol_exists("issetugid" "unistd.h" HAVE_ISSETUGID)
check_symbol_exists("sched_getaffinity" "sched.h" HAVE_SCHED_GETAFFINITY)

set(_Foundation_common_build_flags
    "-DDEPLOYMENT_RUNTIME_SWIFT"
    "-DCF_BUILDING_CF"
    "-DHAVE_STRUCT_TIMESPEC"
    "-Wno-shorten-64-to-32"
    "-Wno-deprecated-declarations"
    "-Wno-unreachable-code"
    "-Wno-conditional-uninitialized"
    "-Wno-unused-variable"
    "-Wno-unused-function"
    "-Wno-microsoft-enum-forward-reference"
    "-Wno-int-conversion"
    "-Wno-switch"
    "-fblocks"
    "-fconstant-cfstrings"
    "-fdollars-in-identifiers"
    "-fno-common"
    "-fcf-runtime-abi=swift"
    "-fexceptions")
foreach(_def HAVE_STRLCAT HAVE_STRLCPY HAVE_ISSETUGID HAVE_SCHED_GETAFFINITY)
  if(${_def})
    list(APPEND _Foundation_common_build_flags "-D${_def}")
  endif()
endforeach()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT APPLE)
  list(APPEND _Foundation_common_build_flags "-D_GNU_SOURCE")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND _Foundation_common_build_flags "-DDEBUG")
endif()

# ---------------------------------------------------------------------------
# The Core Foundation library.
# ---------------------------------------------------------------------------
add_library(CoreFoundation STATIC
    CFApplicationPreferences.c
    CFArray.c
    CFAttributedString.c
    CFBag.c
    CFBase.c
    CFBasicHash.c
    CFBigNumber.c
    CFBinaryHeap.c
    CFBinaryPList.c
    CFBitVector.c
    CFBuiltinConverters.c
    CFBundle_Binary.c
    CFBundle_DebugStrings.c
    CFBundle_Executable.c
    CFBundle_Grok.c
    CFBundle_InfoPlist.c
    CFBundle_Locale.c
    CFBundle_Main.c
    CFBundle_ResourceFork.c
    CFBundle_Resources.c
    CFBundle_SplitFileName.c
    CFBundle_Strings.c
    CFBundle_Tables.c
    CFBundle.c
    CFBurstTrie.c
    CFCalendar_Enumerate.c
    CFCalendar.c
    CFCharacterSet.c
    CFConcreteStreams.c
    CFData.c
    CFDate.c
    CFDateComponents.c
    CFDateFormatter.c
    CFDateInterval.c
    CFDateIntervalFormatter.c
    CFDictionary.c
    CFError.c
    CFFileUtilities.c
    CFICUConverters.c
    CFKnownLocations.c
    CFListFormatter.c
    CFLocale.c
    CFLocaleIdentifier.c
    CFLocaleKeys.c
    CFNumber.c
    CFNumberFormatter.c
    CFOldStylePList.c
    CFPlatform.c
    CFPlatformConverters.c
    CFPlugIn.c
    CFPreferences.c
    CFPropertyList.c
    CFRegularExpression.c
    CFRelativeDateTimeFormatter.c
    CFRunArray.c
    CFRunLoop.c
    CFRuntime.c
    CFSet.c
    CFSocket.c
    CFSocketStream.c
    CFSortFunctions.c
    CFStorage.c
    CFStream.c
    CFString.c
    CFStringEncodingConverter.c
    CFStringEncodingDatabase.c
    CFStringEncodings.c
    CFStringScanner.c
    CFStringTransform.c
    CFStringUtilities.c
    CFSystemDirectories.c
    CFTimeZone.c
    CFTimeZone_WindowsMapping.c
    CFTree.c
    CFUniChar.c
    CFUnicodeDecomposition.c
    CFUnicodePrecomposition.c
    CFURL.c
    CFURLAccess.c
    CFURLComponents_URIParser.c
    CFURLComponents.c
    CFUtilities.c
    CFUUID.c
    CFWindowsUtilities.c
    CFXMLPreferencesDomain.c
    uuid.c)

# The artifact is Agora's: libagora-core.a (the CMake target keeps upstream's name).
set_target_properties(CoreFoundation PROPERTIES OUTPUT_NAME agora-core)

target_compile_definitions(CoreFoundation PRIVATE
  $<$<AND:$<PLATFORM_ID:Windows>,$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>>:CF_WINDOWS_EXECUTABLE_INITIALIZER>)
target_include_directories(CoreFoundation
    PUBLIC
        include
    PRIVATE
        internalInclude)

target_compile_options(CoreFoundation PRIVATE
    "SHELL:$<$<COMPILE_LANGUAGE:C>:${_Foundation_common_build_flags}>")

target_precompile_headers(CoreFoundation PRIVATE internalInclude/CoreFoundation_Prefix.h)

target_link_libraries(CoreFoundation
    PRIVATE
        _FoundationICU
        dispatch
        Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    # On WASI, we use vendored BlocksRuntime instead of the one from libdispatch
    add_subdirectory(BlockRuntime)
    # Add BlocksRuntime object library to CoreFoundation static archive
    target_link_libraries(CoreFoundation PRIVATE BlocksRuntime)
endif()

install(DIRECTORY
            include/
        DESTINATION
            include/CoreFoundation)

install(TARGETS CoreFoundation
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
