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

project(AgoraCore LANGUAGES C OBJC)

# **EVERY LIBRARY FINDS ITS NEIGHBOURS, AND THE INSTALLED BUNDLE NEEDS NO ENVIRONMENT.** A `DT_RUNPATH` is NOT
# inherited by a dependency's own dependencies: whatever a program puts on its search path covers the libraries IT
# names and nothing below them. So an application whose runpath is the prefix still fails to resolve
# `libagora-base.so`'s `libagora-core.so`, because that lookup asks the LIBRARY for its path and it had none —
# MEASURED as an installed bundle that only started with `LD_LIBRARY_PATH` set to the directory its libraries were
# already in. `$ORIGIN` says "the directory I am in", which is where this tree installs every one of them, so the
# chain resolves on its own and a relocatable install is genuinely relocatable.
set(CMAKE_INSTALL_RPATH "\$ORIGIN")

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

find_package(Threads REQUIRED)
find_package(dispatch CONFIG REQUIRED)
find_package(libobjc CONFIG REQUIRED)

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
# AGORA: EVERY LIBRARY IN THIS TREE IS SHARED. A consumer loads the whole .so, so an
# Objective-C category in it is registered whether or not anything references its symbols
# (a static archive drops such members and the categories silently stop answering) and a
# single copy of the runtime state is shared instead of one per binary. See README.md.
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

add_library(_FoundationICU INTERFACE)
target_include_directories(_FoundationICU INTERFACE "$<BUILD_INTERFACE:${_agora_include_dir}>")
target_link_libraries(_FoundationICU INTERFACE ICU::uc ICU::i18n ICU::data)

# ---------------------------------------------------------------------------
# Common build flags. Upstream sets these in the parent Swift build; the Swift
# specific entries are omitted. `-DDEPLOYMENT_RUNTIME_SWIFT` is kept: despite its
# name it selects the non-Darwin code path in Core Foundation (and is also
# defaulted to 1 in internalInclude/CoreFoundation_Prefix.h).
# ---------------------------------------------------------------------------
check_symbol_exists("strlcat" "string.h" HAVE_STRLCAT)
check_symbol_exists("strlcpy" "string.h" HAVE_STRLCPY)
check_symbol_exists("issetugid" "unistd.h" HAVE_ISSETUGID)
check_symbol_exists("sched_getaffinity" "sched.h" HAVE_SCHED_GETAFFINITY)

set(_Foundation_common_build_flags
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
    "-fcf-runtime-abi=objc"
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
add_library(CoreFoundation SHARED
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
    CFHyphenation.c
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
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        internalInclude)

# ---------------------------------------------------------------------------
# THE OBJECTIVE-C DISPATCH PATH. Core Foundation's doors dispatch to bridged
# Objective-C objects (CFInternal.h), and the 172 CF_OBJC_FUNCDISPATCHV and 24
# CF_OBJC_CALLV call sites below are written in Apple's message syntax -- they
# only ever compiled because the macros were no-ops that discarded their
# arguments before the compiler parsed them. Restoring the path means those
# translation units are Objective-C: EXACTLY THOSE, not the library. The rest,
# including the ICU converters and the platform layer, stay C, and the dispatch
# macros stay no-ops in them.
#
# The names are matched against the target's own SOURCES so the property lands on
# the same file CMake is compiling, whichever way the list was written.
#
# CFRuntime.c joins them because its generic doors must hand a bridged object to
# Objective-C (see the CFTYPE_OBJC_FUNCDISPATCH macros there) rather than to Core
# Foundation's own callbacks, which read storage a Foundation object does not have.
# ---------------------------------------------------------------------------
set(_cf_objc_dispatch_sources
    CFRuntime.c
    CFArray.c CFAttributedString.c CFCalendar.c CFCharacterSet.c CFData.c CFDate.c
    CFDictionary.c CFError.c CFLocale.c CFNumber.c CFRunLoop.c CFSet.c CFStream.c
    CFString.c CFTimeZone.c CFURL.c)

get_target_property(_cf_sources CoreFoundation SOURCES)
foreach(_cf_src IN LISTS _cf_sources)
    get_filename_component(_cf_name "${_cf_src}" NAME)
    if(_cf_name IN_LIST _cf_objc_dispatch_sources)
        set_source_files_properties("${_cf_src}" PROPERTIES LANGUAGE OBJC)
    endif()
endforeach()

target_compile_options(CoreFoundation PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-runtime=gnustep-2.0>"
    "$<$<COMPILE_LANGUAGE:OBJC>:-fblocks>")
target_link_libraries(CoreFoundation PRIVATE objc)

target_compile_options(CoreFoundation PRIVATE
    # AGORA: OBJC TOO. These flags carry -DCF_BUILDING_CF and -DCF_HAS_... and the
# dispatch translation units are Objective-C now, so gating them on C alone'
# leaves those units without the build defines -- CFInternal.h refuses to be
# included without CF_BUILDING_CF, which is how this was found.
    "SHELL:$<$<COMPILE_LANGUAGE:C,OBJC>:${_Foundation_common_build_flags}>")

# Core Foundation's headers use blocks, and CFSTR is a compile-time constant, so a
# consumer needs these too: make them part of the usage requirements.
target_compile_options(CoreFoundation INTERFACE
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJC>:-fblocks>"
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJC>:-fconstant-cfstrings>")

target_precompile_headers(CoreFoundation PRIVATE internalInclude/CoreFoundation_Prefix.h)

target_link_libraries(CoreFoundation
    PRIVATE
        _FoundationICU
        dispatch
        Threads::Threads
        m
        ${CMAKE_DL_LIBS})

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    # On WASI, we use vendored BlocksRuntime instead of the one from libdispatch
    add_subdirectory(BlockRuntime)
    # Add BlocksRuntime object library to CoreFoundation static archive
    target_link_libraries(CoreFoundation PRIVATE BlocksRuntime)
endif()

install(DIRECTORY
            include/
        DESTINATION
            ${CMAKE_INSTALL_INCLUDEDIR}/CoreFoundation)

install(TARGETS CoreFoundation _FoundationICU
    EXPORT CoreFoundationTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CMakePackageConfigHelpers)

install(EXPORT CoreFoundationTargets
    FILE CoreFoundationTargets.cmake
    NAMESPACE CoreFoundation::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreFoundation)

configure_package_config_file(cmake/CoreFoundationConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CoreFoundationConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreFoundation)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CoreFoundationConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CoreFoundationConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CoreFoundationConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreFoundation)
