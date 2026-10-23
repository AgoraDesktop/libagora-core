/*	CFKnownLocations.c
	Copyright (c) 1999-2017, Apple Inc. and the Swift project authors
 
	Portions Copyright (c) 2014-2017, Apple Inc. and the Swift project authors
	Licensed under Apache License v2.0 with Runtime Library Exception
	See http://swift.org/LICENSE.txt for license information
	See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
*/

#include "CFKnownLocations.h"

#include "CFString.h"
#include "CFPriv.h"
#include "CFInternal.h"
#include "CFUtilities.h"

#include <assert.h>

#if TARGET_OS_WIN32
#include <userenv.h>
#endif

CFURLRef _Nullable _CFKnownLocationCreatePreferencesURLForUser(CFKnownLocationUser user, CFStringRef _Nullable username) {
    CFURLRef location = NULL;
    
#if TARGET_OS_MAC
    
/*
 Building for a Darwin OS. (We use these paths on Swift builds as well, so that we can interoperate a little with Darwin's defaults(1) command and the other system facilities; but you want to use the system version of CF if possible on those platforms, which will talk to cfprefsd(8) and has stronger interprocess consistency guarantees.)
 
 User:
 - Any: /Library/Preferences
 - Current: $HOME/Library/Preferences
 */
    
    switch (user) {
        case _kCFKnownLocationUserAny:
            location = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, CFSTR("/Library/Preferences"), kCFURLPOSIXPathStyle, true);
            break;
            
        case _kCFKnownLocationUserCurrent:
            username = NULL;
            // passthrough to:
        case _kCFKnownLocationUserByName: {
            /* **THE COMPONENTS ARE APPENDED, NOT SPELLED AS A BASE-RELATIVE PATH.** A file-system path that
             * begins with `/` is ROOT-relative to this tree's CFURL (RFC 3986's rule), so
             * `CFURLCreateWithFileSystemPathRelativeToBase("/Library/Preferences", home)` answers the SYSTEM
             * directory - and the preferences of every user would be the administrator's. Appending the two
             * components asks for the directory inside the home without depending on how a leading slash is
             * read. */
            CFURLRef home = CFCopyHomeDirectoryURLForUser(username);
            CFURLRef library = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, home, CFSTR("Library"), true);
            location = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, library, CFSTR("Preferences"), true);
            CFRelease(library);
            CFRelease(home);
            
            break;
        }
            
    }
#elif !TARGET_OS_WIN32 && !TARGET_OS_ANDROID

/*
 Building for Agora on Linux, the BSDs and anything else that is not Darwin, Windows or Android.
 
 **THE PREFERENCES LIVE IN THE MAC-SHAPED DIRECTORIES, BECAUSE A COMPLETE AGORA INSTALLATION CARRIES THEM.**
 This used to be the FHS/XDG branch (`/usr/local/etc` and `$XDG_CONFIG_PATH`), which is swift-corelibs'
 answer for a system that keeps its files where the FHS says; Agora's Cocoa layer does not. The installation
 creates `/Library/Preferences` and `~/Library/Preferences` (workspace `README.md`, "The installation"), and
 an application that keeps its preferences where the Mac keeps them finds the same directory whether it is
 built for Darwin or for Agora. `_CFXDGCreateConfigHomePath()` remains for the Swift Foundation overlay,
 which has its own FHS parity decision to make; nothing in this stack calls it.

 User:
 - Any: /Library/Preferences
 - Current: $HOME/Library/Preferences
 */
    
    switch (user) {
        case _kCFKnownLocationUserAny:
            location = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, CFSTR("/Library/Preferences"), kCFURLPOSIXPathStyle, true);
            break;
            
        case _kCFKnownLocationUserCurrent:
            username = NULL;
            // passthrough to:
        case _kCFKnownLocationUserByName: {
            /* **THE COMPONENTS ARE APPENDED, NOT SPELLED AS A BASE-RELATIVE PATH.** A file-system path that
             * begins with `/` is ROOT-relative to this tree's CFURL (RFC 3986's rule), so
             * `CFURLCreateWithFileSystemPathRelativeToBase("/Library/Preferences", home)` answers the SYSTEM
             * directory - and the preferences of every user would be the administrator's. Appending the two
             * components asks for the directory inside the home without depending on how a leading slash is
             * read. */
            CFURLRef home = CFCopyHomeDirectoryURLForUser(username);
            CFURLRef library = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, home, CFSTR("Library"), true);
            location = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, library, CFSTR("Preferences"), true);
            CFRelease(library);
            CFRelease(home);
            
            break;
        }
    }
    
#elif TARGET_OS_WIN32

    switch (user) {
        case _kCFKnownLocationUserAny: {
            DWORD size = 0;
            GetAllUsersProfileDirectoryW(NULL, &size);

            wchar_t* path = (wchar_t*)malloc(size * sizeof(wchar_t));
            GetAllUsersProfileDirectoryW(path, &size);

            CFStringRef allUsersPath = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, path, size - 1);
            free(path);

            location = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, allUsersPath, kCFURLWindowsPathStyle, true);
            CFRelease(allUsersPath);
            break;
        }
        case _kCFKnownLocationUserCurrent:
            username = CFGetUserName();
            // fallthrough
        case _kCFKnownLocationUserByName: {
            DWORD size = 0;
            GetProfilesDirectoryW(NULL, &size);

            wchar_t* path = (wchar_t*)malloc(size * sizeof(wchar_t));
            GetProfilesDirectoryW(path, &size);

            CFStringRef pathRef = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, path, size - 1);
            free(path);

            CFURLRef profilesDir = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, pathRef, kCFURLWindowsPathStyle, true);
            CFRelease(pathRef);

            CFURLRef usernameDir = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, profilesDir, username, true);
            CFURLRef appdataDir = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, usernameDir, CFSTR("AppData"), true);
            location = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, appdataDir, CFSTR("Local"), true);
            CFRelease(usernameDir);
            CFRelease(appdataDir);

            CFRelease(profilesDir);
            if (user == _kCFKnownLocationUserCurrent) {
                CFRelease(username);
            }
            break;
        }
    }

#elif TARGET_OS_ANDROID

    // Android doesn't support users, and apps cannot write outside their
    // sandbox. All the preferences will be local to the application, mapping
    // "any user" and other users by name to different directories inside the
    // sandbox.
    CFURLRef userdir = CFCopyHomeDirectoryURL();
    switch (user) {
        case _kCFKnownLocationUserAny:
            location = CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, CFSTR("/Apple/Library/Preferences/AnyUser"), kCFURLPOSIXPathStyle, true, userdir);
            break;
        case _kCFKnownLocationUserByName: {
            CFURLRef tmp = CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, CFSTR("/Apple/Library/Preferences/ByUser"), kCFURLPOSIXPathStyle, true, userdir);
            location = CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, username, kCFURLPOSIXPathStyle, true, tmp);
            CFRelease(tmp);
            break;
        }
        case _kCFKnownLocationUserCurrent:
            location = CFURLCreateWithFileSystemPathRelativeToBase(kCFAllocatorSystemDefault, CFSTR("/Apple/Library/Preferences"), kCFURLPOSIXPathStyle, true, userdir);
            break;
    }
    CFRelease(userdir);

#else
    
    #error For this platform, you need to define a preferences path for both 'any user' (i.e. installation-wide preferences) or the current user.
    
#endif
    
    return location;
}
