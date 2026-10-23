/*	CFSystemDirectories.c
	Copyright (c) 1997-2019, Apple Inc. and the Swift project authors
 
	Portions Copyright (c) 2014-2019, Apple Inc. and the Swift project authors
	Licensed under Apache License v2.0 with Runtime Library Exception
	See http://swift.org/LICENSE.txt for license information
	See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
	Responsibility: Kevin Perry
*/

/*
        This file defines CFCopySearchPathForDirectoriesInDomains().
        On MacOS 8, this function returns empty array.
        On Mach, it calls the System.framework enumeration functions.
        On Windows, it calls the enumeration functions defined here.
*/

#include "CFPriv.h"
#include "CFInternal.h"

#include <string.h>

#if TARGET_OS_MAC

/* We use the System framework implementation on Mach.
*/
#include <libc.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <sysdir.h>

// For now, CFSystemDirectories SPIs are exactly equivalent to (or at least a subset of) sysdir's. NSSearchPath* APIs in Foundation are not a subset of sysdir, so don't attempt to push that functionality down here without accommodating the differences.
#define CFSearchPathToSysdir(dir) ((sysdir_search_path_directory_t)dir)
#define CFSearchPathDomainMaskToSysdir(mask) ((sysdir_search_path_domain_mask_t)domainMask)

CFSearchPathEnumerationState __CFStartSearchPathEnumeration(CFSearchPathDirectory dir, CFSearchPathDomainMask domainMask) {
    return sysdir_start_search_path_enumeration(CFSearchPathToSysdir(dir), CFSearchPathDomainMaskToSysdir(domainMask));
}

CFSearchPathEnumerationState __CFGetNextSearchPathEnumeration(CFSearchPathEnumerationState state, uint8_t *path, CFIndex pathSize) {
    CFSearchPathEnumerationState result;
    // NSGetNextSearchPathEnumeration requires a MAX_PATH size
    if (pathSize < PATH_MAX) {
        uint8_t tempPath[PATH_MAX];
        result = sysdir_get_next_search_path_enumeration(state, (char *)tempPath);
        cf_strlcpy((char *)path, (char *)tempPath, pathSize);
    } else {
        result = sysdir_get_next_search_path_enumeration(state, (char *)path);
    }
    return result;
}

#endif


#if TARGET_OS_MAC

CFArrayRef CFCopySearchPathForDirectoriesInDomains(CFSearchPathDirectory directory, CFSearchPathDomainMask domainMask, Boolean expandTilde) {
    CFMutableArrayRef array;
    CFSearchPathEnumerationState state;
    CFIndex homeLen = -1;
    char cPath[CFMaxPathSize], home[CFMaxPathSize];

    array = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    state = __CFStartSearchPathEnumeration(directory, domainMask);
    while ((state = __CFGetNextSearchPathEnumeration(state, (uint8_t *)cPath, sizeof(cPath)))) {
	CFURLRef url = NULL;
	if (expandTilde && (cPath[0] == '~')) {
	    if (homeLen < 0) {
		CFURLRef homeURL = CFCopyHomeDirectoryURLForUser(NULL);
		if (homeURL) {
		    CFURLGetFileSystemRepresentation(homeURL, true, (uint8_t *)home, CFMaxPathSize);
		    homeLen = strlen(home);
		    CFRelease(homeURL);
		}
	    }
            if (homeLen + strlen(cPath) < CFMaxPathSize) {
		home[homeLen] = '\0';
		cf_strlcat(home, &cPath[1], sizeof(home));
		url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, (uint8_t *)home, strlen(home), true);
	    }
	} else {
	    url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, (uint8_t *)cPath, strlen(cPath), true);
	}
	if (url) {
	    CFArrayAppendValue(array, url);
	    CFRelease(url);
	}
    }
    return array;
}

#elif TARGET_OS_LINUX || TARGET_OS_BSD

/*
 * THE INSTALLATION'S DIRECTORY SET, NAMED ONCE.
 *
 * A complete Agora installation carries the Mac OS X directories (workspace README.md, "The installation"),
 * because the Cocoa surface NAMES its directories instead of searching for them. So the answer for a
 * (directory, domain) pair is a fact about the image rather than a scan of it - the same ground
 * NSTemporaryDirectory() stands on - and this table is that fact in the form both Core Foundation and
 * Foundation ask for it: `NSSearchPathForDirectoriesInDomains()` in libagora-base maps its own enum onto
 * this function, so the two APIs cannot disagree.
 *
 * NULL IN A COLUMN IS A DOMAIN THE DIRECTORY DOES NOT BELONG TO: a Documents folder is the user's,
 * CoreServices is the system's, and answering a path there would be a lie a caller could write into. The
 * developer and demo rows answer nothing at all, because this system has no such directories; they are
 * written down so the omission is a decision rather than an oversight. The NETWORK domain has no tree here
 * (there is no /Network), so every type answers nothing for it.
 *
 * `expandTilde` IS APPLE'S SPELLING RULE FOR THE USER DOMAIN, and it reads the way it is named: true
 * answers the directory EXPANDED (`/Users/x/Library`), false answers `~/Library` - the same directory, and
 * what the Darwin implementation's sysdir enumeration hands back. `-URLsForDirectory:` in Foundation asks
 * for the expanded form, because a URL that begins with a tilde names nothing.
 */
typedef struct {
    CFSearchPathDirectory directory;
    const char *user;	/* relative to the home directory; "" is the home directory itself */
    const char *local;	/* absolute */
    const char *system;	/* absolute */
} _CFSearchPathEntry;

static const _CFSearchPathEntry _CFSearchPathEntries[] = {
    { kCFApplicationDirectory,		"Applications",				"/Applications",			"/Applications" },
    { kCFDemoApplicationDirectory,	NULL,					NULL,					NULL },
    { kCFDeveloperApplicationDirectory,	NULL,					NULL,					NULL },
    { kCFAdminApplicationDirectory,	NULL,					NULL,					NULL },
    { kCFLibraryDirectory,		"Library",				"/Library",				"/System/Library" },
    { kCFDeveloperDirectory,		NULL,					NULL,					NULL },
    { kCFUserDirectory,			"",					"/Users",				"/Users" },
    { kCFDocumentationDirectory,	"Library/Documentation",		"/Library/Documentation",		"/System/Library/Documentation" },
    { kCFDocumentDirectory,		"Documents",				NULL,					NULL },
    { kCFCoreServiceDirectory,		NULL,					NULL,					"/System/Library/CoreServices" },
    { kCFAutosavedInformationDirectory,	"Library/Autosave",			NULL,					NULL },
    { kCFDesktopDirectory,		"Desktop",				NULL,					NULL },
    { kCFCachesDirectory,		"Library/Caches",			"/Library/Caches",			"/System/Library/Caches" },
    { kCFApplicationSupportDirectory,	"Library/Application Support",		"/Library/Application Support",		"/System/Library/Application Support" },
    { kCFDownloadsDirectory,		"Downloads",				NULL,					NULL },
    { kCFInputMethodsDirectory,		"Library/Input Methods",		"/Library/Input Methods",		"/System/Library/Input Methods" },
    { kCFMoviesDirectory,		"Movies",				NULL,					NULL },
    { kCFMusicDirectory,		"Music",				NULL,					NULL },
    { kCFPicturesDirectory,		"Pictures",				NULL,					NULL },
    { kCFPrinterDescriptionDirectory,	"Library/Printers/PPDs",		"/Library/Printers/PPDs",		"/System/Library/Printers/PPDs" },
    { kCFSharedPublicDirectory,		"Public",				NULL,					NULL },
    { kCFPreferencePanesDirectory,	"Library/PreferencePanes",		"/Library/PreferencePanes",		"/System/Library/PreferencePanes" },
    { kCFApplicationScriptsDirectory,	"Library/Application Scripts",		NULL,					NULL },
    /* THE USER'S TRASH IS A DIRECTORY EVEN WHERE MOVING TO IT IS NOT IMPLEMENTED: the Dock draws `~/.Trash`
     * as the can, so the location exists while the move does not (Foundation's -trashItemAtURL: says so). */
    { kCFTrashDirectory,		".Trash",				NULL,					NULL },
};

/* THE HOME DIRECTORY, OR NULL. One lookup per call rather than a cached copy: the answer is THIS process's
 * account, and the account database is what NSHomeDirectory() reads too. */
static CFStringRef _CFSearchPathCopyHome(void) {
    CFURLRef homeURL = CFCopyHomeDirectoryURLForUser(NULL);
    CFStringRef home = NULL;

    if (homeURL != NULL) {
        home = CFURLCopyFileSystemPath(homeURL, kCFURLPOSIXPathStyle);
        CFRelease(homeURL);
    }
    return home;
}

static CFStringRef _CFSearchPathCopyPathForHome(CFStringRef home, const char *relative) {
    if (relative == NULL || home == NULL) return NULL;
    if (relative[0] == '\0') return (CFStringRef)CFRetain(home);
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("%@/%s"), home, relative);
}

/* `~/Library` IS A SPELLING OF `/Users/<name>/Library`, and `expandTilde` says which one a caller gets: true
 * keeps the path expanded, false shortens a path under the home directory. The SAME directory either way. */
static CFStringRef _CFSearchPathCopySpelling(CFStringRef path, CFStringRef home, Boolean expandTilde) {
    CFIndex pathLength, homeLength;

    if (path == NULL) return NULL;
    if (expandTilde || home == NULL) return (CFStringRef)CFRetain(path);
    if (CFEqual(path, home)) {
        return (CFStringRef)CFRetain(CFSTR("~"));
    }
    pathLength = CFStringGetLength(path);
    homeLength = CFStringGetLength(home);
    if (pathLength > homeLength &&
        CFStringHasPrefix(path, home) &&
        CFStringGetCharacterAtIndex(path, homeLength) == '/') {
        CFStringRef tail = CFStringCreateWithSubstring(kCFAllocatorSystemDefault, path, CFRangeMake(homeLength, pathLength - homeLength));
        CFStringRef spelling = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("~%@"), tail);

        CFRelease(tail);
        return spelling;
    }
    return (CFStringRef)CFRetain(path);
}

/* APPENDED ONCE: with no disk arbitration the local and system domains name the same /Applications, and a
 * caller iterating the list does not want it twice. `seen` holds the SPELLINGS, because two spellings are
 * two answers even when they name one directory (`~/Library` and `/Users/x/Library`) - but one spelling is
 * one answer, which is the case the check is for. */
static void _CFSearchPathAppend(CFMutableArrayRef array, CFMutableArrayRef seen, CFStringRef path, CFStringRef home, Boolean expandTilde) {
    CFStringRef spelling;
    CFURLRef url;

    if (path == NULL) return;
    spelling = _CFSearchPathCopySpelling(path, home, expandTilde);
    if (spelling == NULL) return;
    if (CFArrayContainsValue(seen, CFRangeMake(0, CFArrayGetCount(seen)), spelling)) {
        CFRelease(spelling);
        return;
    }
    CFArrayAppendValue(seen, spelling);
    url = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, spelling, kCFURLPOSIXPathStyle, true);
    CFRelease(spelling);
    if (url != NULL) {
        CFArrayAppendValue(array, url);
        CFRelease(url);
    }
}

static void _CFSearchPathAppendAbsolute(CFMutableArrayRef array, CFMutableArrayRef seen, const char *path, CFStringRef home, Boolean expandTilde) {
    CFStringRef string;

    if (path == NULL) return;
    string = CFStringCreateWithCString(kCFAllocatorSystemDefault, path, kCFStringEncodingUTF8);
    _CFSearchPathAppend(array, seen, string, home, expandTilde);
    if (string != NULL) CFRelease(string);
}

CFArrayRef CFCopySearchPathForDirectoriesInDomains(CFSearchPathDirectory directory, CFSearchPathDomainMask domainMask, Boolean expandTilde) {
    /* APPLE'S DOMAIN ORDER: the user's directory first, then the machine's, then the network's, then the
     * system's - so a caller taking the first answer takes the most specific one. */
    static const CFSearchPathDomainMask order[4] = {
        kCFUserDomainMask, kCFLocalDomainMask, kCFNetworkDomainMask, kCFSystemDomainMask
    };
    CFMutableArrayRef array = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFMutableArrayRef seen = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFStringRef home = _CFSearchPathCopyHome();
    CFIndex i;

    for (i = 0; i < 4; i++) {
        CFSearchPathDomainMask domain = order[i];
        size_t j;

        if ((domainMask & domain) == 0) continue;

        /* THE TWO UNION TYPES ARE COMPOSED FROM THE SAME DIRECTORIES THE TABLE NAMES, so they cannot list a
         * directory the rest of the function does not answer. */
        if (directory == kCFAllApplicationsDirectory) {
            if (domain == kCFUserDomainMask) {
                CFStringRef path = _CFSearchPathCopyPathForHome(home, "Applications");

                _CFSearchPathAppend(array, seen, path, home, expandTilde);
                if (path != NULL) CFRelease(path);
            } else if (domain == kCFLocalDomainMask || domain == kCFSystemDomainMask) {
                _CFSearchPathAppendAbsolute(array, seen, "/Applications", home, expandTilde);
                _CFSearchPathAppendAbsolute(array, seen, "/Applications/Utilities", home, expandTilde);
            }
            continue;
        }
        if (directory == kCFAllLibrariesDirectory) {
            if (domain == kCFUserDomainMask) {
                CFStringRef path = _CFSearchPathCopyPathForHome(home, "Library");

                _CFSearchPathAppend(array, seen, path, home, expandTilde);
                if (path != NULL) CFRelease(path);
            } else if (domain == kCFLocalDomainMask) {
                _CFSearchPathAppendAbsolute(array, seen, "/Library", home, expandTilde);
            } else if (domain == kCFSystemDomainMask) {
                _CFSearchPathAppendAbsolute(array, seen, "/System/Library", home, expandTilde);
            }
            continue;
        }

        for (j = 0; j < sizeof _CFSearchPathEntries / sizeof _CFSearchPathEntries[0]; j++) {
            const _CFSearchPathEntry *entry = &_CFSearchPathEntries[j];

            if (entry->directory != directory) continue;
            if (domain == kCFUserDomainMask) {
                CFStringRef path = _CFSearchPathCopyPathForHome(home, entry->user);

                _CFSearchPathAppend(array, seen, path, home, expandTilde);
                if (path != NULL) CFRelease(path);
            } else if (domain == kCFLocalDomainMask) {
                _CFSearchPathAppendAbsolute(array, seen, entry->local, home, expandTilde);
            } else if (domain == kCFSystemDomainMask) {
                _CFSearchPathAppendAbsolute(array, seen, entry->system, home, expandTilde);
            }
            /* the network domain has no tree here */
            break;
        }
    }
    if (home != NULL) CFRelease(home);
    CFRelease(seen);
    return array;
}

#endif


#undef numDirs
#undef numApplicationDirs
#undef numLibraryDirs
#undef numDomains
#undef invalidDomains
#undef invalidDomains
