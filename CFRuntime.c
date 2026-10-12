/*	CFRuntime.c
	Copyright (c) 1999-2019, Apple Inc. and the Swift project authors
 
	Portions Copyright (c) 2014-2019, Apple Inc. and the Swift project authors
	Licensed under Apache License v2.0 with Runtime Library Exception
	See http://swift.org/LICENSE.txt for license information
	See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
	Responsibility: Michael LeHew
*/

#define ENABLE_ZOMBIES 1

#include "CFBase.h"
#include "CFRuntime.h"
#include "CFRuntime_Internal.h"
#include "CFInternal.h"
#include "CFBasicHash.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "CFUUID.h"
#include "CFCalendar.h"
#include "CFURLComponents.h"
#if TARGET_OS_MAC
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <crt_externs.h>
#include <unistd.h>
#include <sys/stat.h>
#include "CFStringDefaultEncoding.h"
#endif
#include "CFUUID.h"
#include "CFTimeZone.h"
#include "CFCalendar.h"
#include "CFAttributedString.h"	/* the owned class for CFAttributedString answers from its two doors */
#if TARGET_OS_IPHONE
// This isn't in the embedded runtime.h header
OBJC_EXPORT void *objc_destructInstance(id obj);
#endif


#if TARGET_OS_WIN32
#include <Shellapi.h>
#include <pathcch.h>
#endif

#if __HAS_DISPATCH__ && !TARGET_OS_MAC
#include <dispatch/dispatch.h>
#endif

enum {
// retain/release recording constants -- must match values
// used by OA for now; probably will change in the future
__kCFRetainEvent = 28,
__kCFReleaseEvent = 29
};

#if TARGET_OS_WIN32 || TARGET_OS_LINUX || TARGET_OS_WASI
#include <malloc.h>
#elif TARGET_OS_BSD
#include <stdlib.h> // malloc()
#else
#include <malloc/malloc.h>
#endif

#define FAKE_INSTRUMENTS 0

#if TARGET_OS_MAC
CF_PRIVATE void __CFOAInitializeNSObject(void);  // from NSObject.m

bool __CFOASafe = false;

void (*__CFObjectAllocRecordAllocationFunction)(int, void *, int64_t , uint64_t, const char *) = NULL;
void (*__CFObjectAllocSetLastAllocEventNameFunction)(void *, const char *) = NULL;

void __CFOAInitialize(void) {
}

void __CFRecordAllocationEvent(int eventnum, void *ptr, int64_t size, uint64_t data, const char *classname) {
    if (!__CFOASafe || !__CFObjectAllocRecordAllocationFunction) return;
    __CFObjectAllocRecordAllocationFunction(eventnum, ptr, size, data, classname);
}

void __CFSetLastAllocationEventName(void *ptr, const char *classname) {
    if (!__CFOASafe || !__CFObjectAllocSetLastAllocEventNameFunction) return;
    __CFObjectAllocSetLastAllocEventNameFunction(ptr, classname);
}

#elif FAKE_INSTRUMENTS

CF_EXPORT bool __CFOASafe = true;

void __CFOAInitialize(void) { }

void __CFRecordAllocationEvent(int eventnum, void *ptr, int64_t size, uint64_t data, const char *classname) {
    if (!__CFOASafe) return;
    if (!classname) classname = "(no class)";
    const char *event = "unknown event";
    switch (eventnum) {
        case 21:
            event = "zombie";
            break;
        case 13:
        case __kCFReleaseEvent:
            event = "release";
            break;
        case 12:
        case __kCFRetainEvent:
            event = "retain";
            break;
    }
    fprintf(stdout, "event,%d,%s,%p,%ld,%lu,%s\n", eventnum, event, ptr, (long)size, (unsigned long)data, classname);
}

void __CFSetLastAllocationEventName(void *ptr, const char *classname) {
    if (!__CFOASafe) return;
    if (!classname) classname = "(no class)";
    fprintf(stdout, "name,%p,%s\n", ptr, classname ? classname : "(no class)");
}

#else

bool __CFOASafe = false;

void __CFOAInitialize(void) { }

#endif

extern void __HALT(void);

#if !defined (__cplusplus)
static const CFRuntimeClass __CFNotATypeClass = {
    0,
    "Not A Type",
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT
};

static const CFRuntimeClass __CFTypeClass = {
    0,
    "CFType",
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT,
    (void *)__HALT
};
#else
void SIG1(CFTypeRef){__HALT();};;
CFTypeRef SIG2(CFAllocatorRef,CFTypeRef){__HALT();return NULL;};
Boolean SIG3(CFTypeRef,CFTypeRef){__HALT();return FALSE;};
CFHashCode SIG4(CFTypeRef){__HALT(); return 0;};
CFStringRef SIG5(CFTypeRef,CFDictionaryRef){__HALT();return NULL;};
CFStringRef SIG6(CFTypeRef){__HALT();return NULL;};

static const CFRuntimeClass __CFNotATypeClass = {
    0,
    "Not A Type",
    SIG1,
    SIG2,
    SIG1,
    SIG3,
    SIG4,
    SIG5,
    SIG6
};

static const CFRuntimeClass __CFTypeClass = {
    0,
    "CFType",
    SIG1,
    SIG2,
    SIG1,
    SIG3,
    SIG4,
    SIG5,
    SIG6
};
#endif //__cplusplus

// the lock does not protect most reading of these; we just leak the old table to allow read-only accesses to continue to work
static os_unfair_lock __CFBigRuntimeFunnel = OS_UNFAIR_LOCK_INIT;

_CFClassTables __CFRuntimeClassTables __attribute__((aligned)) = {
    .classTable = {
    [_kCFRuntimeIDNotAType] = &__CFNotATypeClass,
    [_kCFRuntimeIDCFType] = &__CFTypeClass,
    [_kCFRuntimeIDCFAllocator] = &__CFAllocatorClass,
    [_kCFRuntimeIDCFBasicHash] = &__CFBasicHashClass,
    [_kCFRuntimeIDCFBag] = &__CFBagClass,
    [_kCFRuntimeIDCFString] = &__CFStringClass,
    [_kCFRuntimeIDCFNull] = &__CFNullClass,
    [_kCFRuntimeIDCFSet] = &__CFSetClass,
    [_kCFRuntimeIDCFDictionary] = &__CFDictionaryClass,
    [_kCFRuntimeIDCFArray] = &__CFArrayClass,
    [_kCFRuntimeIDCFData] = &__CFDataClass,
    [_kCFRuntimeIDCFBoolean] = &__CFBooleanClass,
    [_kCFRuntimeIDCFNumber] = &__CFNumberClass,
    [_kCFRuntimeIDCFBinaryHeap] = &__CFBinaryHeapClass,
    [_kCFRuntimeIDCFBitVector] = &__CFBitVectorClass,
    [_kCFRuntimeIDCFUUID] = &__CFUUIDClass,
    [_kCFRuntimeIDCFStorage] = &__CFStorageClass,
    [_kCFRuntimeIDCFTree] = &__CFTreeClass,
    [_kCFRuntimeIDCFError] = &__CFErrorClass,
    [_kCFRuntimeIDCFLocale] = &__CFLocaleClass,
    [_kCFRuntimeIDCFDateFormatter] = &__CFDateFormatterClass,
    [_kCFRuntimeIDCFNumberFormatter] = &__CFNumberFormatterClass,
    [_kCFRuntimeIDCFCalendar] = &__CFCalendarClass,
    [_kCFRuntimeIDCFDateIntervalFormatter] = &__CFDateIntervalFormatterClass,
    [_kCFRuntimeIDCFDate] = &__CFDateClass,
    [_kCFRuntimeIDCFTimeZone] = &__CFTimeZoneClass,
    [_kCFRuntimeIDCFKeyedArchiverUID] = &__CFKeyedArchiverUIDClass,
    
#if TARGET_OS_OSX && DEPLOYMENT_RUNTIME_OBJC
    [_kCFRuntimeIDCFXMLParser] = &__CFXMLParserClass,
    [_kCFRuntimeIDCFXMLNode] = &__CFXMLNodeClass,
#endif // TARGET_OS_OSX
    
    [_kCFRuntimeIDCFBundle] = &__CFBundleClass,
    [_kCFRuntimeIDCFPFactory] = &__CFPFactoryClass,
    [_kCFRuntimeIDCFPlugInInstance] = &__CFPlugInInstanceClass,

    [_kCFRuntimeIDCFPreferencesDomain] = &__CFPreferencesDomainClass,

#if TARGET_OS_MAC
    [_kCFRuntimeIDCFMachPort] = &__CFMachPortClass,
#endif



#if !TARGET_OS_WASI
    [_kCFRuntimeIDCFRunLoopMode] = &__CFRunLoopModeClass,
    [_kCFRuntimeIDCFRunLoop] = &__CFRunLoopClass,
    [_kCFRuntimeIDCFRunLoopSource] = &__CFRunLoopSourceClass,
    [_kCFRuntimeIDCFRunLoopObserver] = &__CFRunLoopObserverClass,
    [_kCFRuntimeIDCFRunLoopTimer] = &__CFRunLoopTimerClass,
    [_kCFRuntimeIDCFSocket] = &__CFSocketClass,
#endif
    [_kCFRuntimeIDCFReadStream] = &__CFReadStreamClass,
    [_kCFRuntimeIDCFWriteStream] = &__CFWriteStreamClass,
    [_kCFRuntimeIDCFAttributedString] = &__CFAttributedStringClass,
    [_kCFRuntimeIDCFRunArray] = &__CFRunArrayClass,
    [_kCFRuntimeIDCFCharacterSet] = &__CFCharacterSetClass,
    
    
    [_kCFRuntimeIDCFURL] = &__CFURLClass,

    
    [_kCFRuntimeIDCFURLComponents] = &__CFURLComponentsClass,
    [_kCFRuntimeIDCFDateComponents] = &__CFDateComponentsClass,
    [_kCFRuntimeIDCFRelativeDateTimeFormatter] = &__CFRelativeDateTimeFormatterClass,
    [_kCFRuntimeIDCFListFormatter] = &__CFListFormatterClass,
    },
    .objCClassTable = {0}
};

static int32_t __CFRuntimeClassTableCount = _kCFRuntimeStartingClassID;

#if (TARGET_OS_MAC && !TARGET_OS_IPHONE && !__x86_64h__) // Match parity with private header
bool (*__CFObjCIsCollectable)(void *) = NULL;
#else
#endif

#if DEPLOYMENT_RUNTIME_SWIFT
// The constant string class reference is set at link time to _NSCFConstantString
void *__CFConstantStringClassReferencePtr = &_CF_CONSTANT_STRING_SWIFT_CLASS;
#else
#ifndef __CONSTANT_CFSTRINGS__
// Compiler uses this symbol name; must match compiler built-in decl, so we use 'int'
#if TARGET_RT_64_BIT
int __CFConstantStringClassReference[24] = {0};
#else
int __CFConstantStringClassReference[12] = {0};
#endif
#endif

/* AGORA: A WEAK DEFINITION, SO THE FOUNDATION SIDE CAN SUPPLY THE CLASS.
 *
 * clang emits every CFSTR as a static struct whose isa is &__CFConstantStringClassReference -- a BARE
 * SYMBOL, not a class symbol -- and the layout is clang's: {isa, uint32 flags, const char *cstr,
 * uintptr length}, 32 bytes. For the object to be a real Foundation string, that symbol's storage must
 * BE the class, and only one translation unit can define it. Upstream has the same arrangement for the
 * Swift runtime: CFString.h declares `_CF_CONSTANT_STRING_SWIFT_CLASS` and the Foundation side defines
 * it. The non-Swift path had no one playing Foundation's part, so the blob stayed zeroed and any class
 * operation on a CFSTR segfaulted.
 *
 * Weakly defined here so that libagora-base's strong definition -- its constant-string class for exactly
 * clang's layout -- wins at load, and so that a Core Foundation with no Foundation beside it still links
 * and still gets a zeroed placeholder rather than a missing symbol. */
#if TARGET_RT_64_BIT
__attribute__((weak)) int __CFConstantStringClassReference[24] = {0};
#else
__attribute__((weak)) int __CFConstantStringClassReference[12] = {0};
#endif

void *__CFConstantStringClassReferencePtr = NULL;
#endif

/* AGORA: WHETHER AN OBJECT IS AN OBJECTIVE-C INSTANCE OF THE CLASS A CORE FOUNDATION TYPE BRIDGES TO,
 * and THE COMPARISON IS BY SUBCLASS RATHER THAN BY EQUALITY.
 *
 * Apple's function here is a wrapper around the macro, and the macro asks the equality question --
 * `_CFTypeGetClass(obj) == __CFISAForTypeID(typeID)`. That is the shape upstream needs: under the Swift
 * runtime every bridged object is an instance of ONE class, so one slot per type id matches.
 *
 * LIBAGORA-BASE IS A CLASS CLUSTER, SO EQUALITY MATCHES NOTHING. `@[]` is an AGArrayEmpty, `@[@1]` an
 * AGArrayOne, `@[@1,@2]` an AGArraySmall and `[NSMutableArray array]` an AGArrayMutable; libagora-base
 * alone declares AGArrayEmpty, AGArrayItems, AGArrayMutable, AGArrayOne and AGArraySmall against
 * CFArrayGetTypeID(), and the same holds for data, dictionaries, sets, ordered sets, numbers and strings.
 * `_CFRuntimeBridgeTypeToClass` has one slot per type id and Foundation registers the cluster's ROOT, so
 * the question with an answer is whether the object's class chain reaches that root.
 *
 * `class_getSuperclass` IS THE RUNTIME'S OWN DOOR, declared here rather than included because this
 * translation unit is C and Core Foundation's build puts the runtime's headers on the path only for its
 * Objective-C units. It is an ordinary C function in libobjc2 with the obvious signature.
 */
#if !defined(__OBJC__)
/* Declared only for the C build: with __OBJC__ the runtime's own headers are in view and
 * these are the same functions. */
extern void *class_getSuperclass(void *cls);
#endif

/* THE RUNTIME'S OWN ACCESSOR, AND READING THE ISA DIRECTLY IS WHAT IT REPLACES. A SMALL OBJECT -- a
 * tagged pointer, which is what a short @"..." literal is -- has no isa at all: libobjc2's
 * isSmallObject() is `(addr & OBJC_SMALL_OBJECT_MASK) != 0` (mask 7 on 64-bit) and the class is looked
 * up from the pointer's tag bits instead. Reading offset 0 of such a pointer dereferences
 * 0xd19766cde000002c and dies, which is exactly the crash this replaced: measured in _CFIsObjC at
 * `mov (%rsi),%rax`, with the pointer in %rbx.
 *
 * object_getClass is what the runtime's own header recommends over reading isa, and it decodes both
 * shapes. The chain walk stays because libagora-base is a cluster, and both accessors are declared here
 * rather than included because this translation unit is C and Core Foundation's own build gives its C
 * units `id` as `char *` -- which the runtime's header would redefine. */
#if !defined(__OBJC__)
/* Declared only for the C build: with __OBJC__ the runtime's own headers are in view and
 * these are the same functions. */
extern void *object_getClass(void *obj);
#endif

/* AGORA: WHICH TYPE ID A FOUNDATION OBJECT ANSWERS TO -- the question CFGetTypeID asks, and the inverse
 * of the one _CFIsObjC asks. CFGetTypeID is the most basic door Core Foundation has, and it called
 * CF_IS_OBJC-adjacent machinery that is stubbed here, then read the type word at offset 0: on a tagged
 * pointer that dereferences 0xd19766cde000002c and dies. (Found by libagora-base's test suite on its
 * first run, with the pointer in rdi.)
 *
 * Apple's shape is the Objective-C DISPATCH macro above -- `[(id)cf _cfTypeID]`, with Foundation
 * implementing -_cfTypeID per class. Agora asks instead, because the answer is already in the table this
 * file owns: walk the object's class chain and return the type id whose bridge class it reaches. Same
 * machinery as _CFIsObjC below, one pass over the registered types, and NO FOUNDATION METHOD NEEDED --
 * which matters because this translation unit is C and a message send is not available to it.
 *
 * A Core Foundation object returns _kCFRuntimeNotATypeID: it may wear __NSCFType (see
 * _CFRuntimeCreateInstance), whose chain reaches no bridge root, so no type id is claimed. */
/* AGORA: IS THIS POINTER AN OBJECTIVE-C OBJECT AT ALL? Cheap, and it needs no type id -- which matters,
 * because CFRetain and CFRelease ask it on every call. A CORE FOUNDATION OBJECT WEARS A CF-OWNED CLASS
 * (see _CFRuntimeCreateInstance) -- __NSCFType when its type has none of its own -- so it IS an object by
 * this test; a Foundation object has an isa too, and a small object has tag bits the runtime decodes.
 * The test says "has a named class", NOT "is Foundation's": _CFIsOwnedCFObject is the second question,
 * and it is the one CFRetain and CFRelease act on. */
/* THE CLASS CORE FOUNDATION MADE FOR ITSELF, per type id, alongside the class Foundation registered.
 * Two tables because the two questions are different ones: CF_IS_OBJC asks about Foundation's cluster,
 * _CFRuntimeCreateInstance asks what a Core Foundation object should wear. */
#if defined(__OBJC__)
static uintptr_t __CFRuntimeCFOwnClassTable[__CFRuntimeClassTableSize];
/* THE MUTABLE HALF'S CLASS, keyed by the SAME type id -- the two halves share it, which is exactly why an
 * instance's class cannot come from the type id and the creation category says it instead. */
static uintptr_t __CFRuntimeCFOwnMutableClassTable[__CFRuntimeClassTableSize];

/* AGORA: THE SAME CLASSES, AS A LIST. CFRetain and CFRelease have to ask "is this object Core
 * Foundation's own?", and they ask it on EVERY call, so the answer cannot be a walk of the type-id
 * table -- 1024 entries deep on the hot path. Filled by cf_bridged_subclass below, and short: one entry
 * per type that has an owned class. */
#define __CFRuntimeMaxOwnedClasses 32
static uintptr_t __CFRuntimeOwnedClasses[__CFRuntimeMaxOwnedClasses];
static size_t __CFRuntimeOwnedClassCount;
#endif

CF_PRIVATE Boolean _CFIsAnObject(const void *obj) {
    Class cls;

    /* AND THE CLASS HAS TO BE ONE THE RUNTIME KNOWS, WHICH IS THE HALF THAT WAS MISSING. `object_getClass`
     * answers a POINTER for anything with an isa, including a bare CFSTR: clang gives every constant string
     * the placeholder storage declared above as its isa, and that storage is zeroed, so the pointer is not a
     * class there is anything to send to. MEASURED, WITH NO FOUNDATION LOADED AT ALL:
     *
     *   object_getClass(CFSTR("kCGColorSpaceSRGB"))          -> non-NULL  (which is why this test passed)
     *   class_getName(object_getClass(CFSTR("...")))         -> NULL      (a class without a name)
     *   CFEqual(CFSTR("kCGColorSpaceSRGB"), CFSTR("kCGColorSpaceAdobeRGB1998"))
     *     -> objc_msgSend_fpret <- CFEqual, rsi = "isEqual:", rdi = the constant, SIGSEGV
     *
     * WHAT IT COST: that is the first comparison of two DISTINCT Core Foundation constants in
     * libagora-drawing, which is how CGColorSpaceCreateWithName came to segfault on kCGColorSpaceAdobeRGB1998
     * while kCGColorSpaceSRGB worked -- `CFEqual` returns early when the two pointers are IDENTICAL, so the
     * single-name case never reached a dispatch at all.
     *
     * A BRIDGED FOUNDATION OBJECT IS UNAFFECTED: its class is one the runtime registered and it has a name,
     * so it is an object by this test and the doors hand it to Objective-C exactly as before. */
    if (obj == NULL) {
        return false;
    }
    cls = object_getClass((void *)obj);
    return cls != NULL && class_getName(cls) != NULL;
}

/* AGORA: IS THIS OBJECT ONE CORE FOUNDATION MADE, RATHER THAN ONE FOUNDATION DID? Both are objects, and
 * CFRetain/CFRelease must treat them differently -- see the note on CFRetain. MEASURED, AND IT IS WHY THIS
 * EXISTS: libobjc2 keeps its retain count at ((uintptr_t *)obj)[-1], which for a CFRuntime allocation is
 * the MALLOC CHUNK HEADER. A probe over CFArray/CFString/CFData/CFDictionary/CFSet read 0x41, 0x31, 0x51,
 * 0x51, 0x41 before the retain and saw each word INCREMENTED by CFRetain and decremented by CFRelease --
 * heap metadata rewritten, and Core Foundation's own reference count never touched, so no Core Foundation
 * object wearing an owned class was ever freed. */
CF_PRIVATE Boolean _CFIsOwnedCFObject(const void *obj) {
#if defined(__OBJC__)
    uintptr_t cls;
    size_t i;

    if (obj == NULL) {
        return false;
    }
    cls = (uintptr_t)object_getClass((void *)obj);
    if (cls == 0) {
        return false;
    }
    for (i = 0; i < __CFRuntimeOwnedClassCount; i++) {
        if (__CFRuntimeOwnedClasses[i] == cls) {
            return true;
        }
    }
    return false;
#else
    (void)obj;
    return false;
#endif
}

CF_PRIVATE CFTypeID _CFBridgedTypeIDForObject(const void *obj) {
    uintptr_t cls;
    CFTypeID type;

    if (obj == NULL) {
        return _kCFRuntimeNotATypeID;
    }
    cls = (uintptr_t)object_getClass((void *)obj);
    if (cls == 0) {
        return _kCFRuntimeNotATypeID;
    }
    for (type = 1; type < (CFTypeID)__CFRuntimeClassTableSize; type++) {
        uintptr_t want = __CFISAForTypeID(type);
        uintptr_t c;

        if (want == 0) {
            continue;
        }
        for (c = cls; c != 0; c = (uintptr_t)class_getSuperclass((void *)c)) {
            if (c == want) {
                return type;
            }
        }
    }
    return _kCFRuntimeNotATypeID;
}

#if defined(__OBJC__)
/* AGORA: A CORE FOUNDATION OBJECT WEARS A CORE FOUNDATION CLASS -- the last piece of the bridge, and
 * Apple's own answer to a problem this file created twice.
 *
 * It cannot wear FOUNDATION'S class: CFArrayCreate would come back as an NSArray and CFArrayAppendValue
 * would dispatch into an immutable one. It cannot wear NO class either, which is where this stood until
 * now: measured as SIGSEGV in CFEqual(@"same", aNativeCFString), because Foundation's -isEqual: receives
 * the Core Foundation object as an ARGUMENT and has nothing to message.
 *
 * SO IT WEARS A SUBCLASS OF FOUNDATION'S, MADE BY THE RUNTIME AND OWNED BY CORE FOUNDATION, WHOSE METHODS
 * ARE CORE FOUNDATION'S OWN. That is Apple's __NSCFString, and it works here because
 * objc_allocateClassPair takes the class Foundation handed over and class_addMethod takes a C function --
 * verified before any of this was written. Three consequences, all intended:
 *   - a Core Foundation string IS an NSString, so Foundation's -isEqual: can read it;
 *   - its -length is Core Foundation's CFStringGetLength, which does NOT recurse, because _CFIsObjC
 *     refuses this class exactly as it refuses no class at all;
 *   - CFArrayAppendValue still reaches Core Foundation, because the class provides Core Foundation's
 *     implementation rather than Foundation's.
 *
 * THE MINIMUM METHOD SET IS MEASURED, NOT GUESSED: -length and -characterAtIndex: are what Foundation's
 * -isEqual: actually sends to its argument, and the probe that established the mechanism also established
 * that the encoding must match the method being replaced -- Q@: for -length, S@:Q for -characterAtIndex:.
 * libobjc2 reports 'incorrect signature' when it does not match, rather than failing silently. */
extern Class objc_allocateClassPair(Class superclass, const char *name, size_t extraBytes);
extern Class objc_getClass(const char *name);
extern BOOL class_addMethod(Class cls, SEL name, IMP imp, const char *types);
extern void objc_registerClassPair(Class cls);

static unsigned long cf_imp_string_length(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFStringGetLength((CFStringRef)self);
}

static unsigned short cf_imp_string_char_at(id self, SEL _cmd, unsigned long index)
{
	(void)_cmd;
	return (unsigned short)CFStringGetCharacterAtIndex((CFStringRef)self, (CFIndex)index);
}

/* THE THIRD AND LAST ONE. Base NSString is abstract and refuses -length, -characterAtIndex: and
 * -UTF8String with a message that names itself -- "a bare NSString holds no characters" -- so a direct
 * subclass inherits the REFUSAL rather than an implementation. The three are therefore not "what
 * Foundation sends this class"; they are the abstract methods of the class above it, and the set closes
 * at three. This answers with Core Foundation bytes: CFStringGetCStringPtr returns the internal buffer
 * for an eight-bit string, which is what a CFString built from a C string has, and the pointer is valid
 * for as long as the string is. */
static const char *cf_imp_string_utf8(id self, SEL _cmd)
{
	(void)_cmd;
	return CFStringGetCStringPtr((CFStringRef)self, kCFStringEncodingUTF8);
}

/* ---------------------------------------------------------------------------
 * THE TABLE OF CORE FOUNDATION-OWNED CLASSES, AND THE LESSON IS THAT IT IS SHORT.
 *
 * Foundation's classes are CLUSTERS, and the method counts look alarming until you read where they
 * live: NSArray implements 127 methods, but its concrete classes implement almost none -- AGArrayEmpty
 * 8, AGArrayItems 1, AGArrayMutable 0 -- because THE OTHER ~110 RIDE ON THE PRIMITIVES. A Core
 * Foundation-owned class therefore needs ONLY THE PRIMITIVES, and inherits the derived ones, which is
 * exactly the arrangement Apple relies on. The real cost is Core Foundation's own dispatch list, not
 * Foundation's public API.
 *
 * EVERY IMP CALLS CORE FOUNDATION'S DOOR FOR THAT METHOD, and it cannot recurse: _CFIsObjC refuses this
 * class, so the door takes its native path. THE ENCODING MUST MATCH THE METHOD IT REPLACES -- measured,
 * twice, as libobjc2's 'incorrect signature' -- and a wrong one is a silently wrong answer rather than a
 * loud failure, which is how -_secondsFromGMTForAbsoluteTime: returning NSInteger cost an afternoon.
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * THE TABLE OF CORE FOUNDATION-OWNED CLASSES, AND THE LESSON IS THAT IT IS SHORT.
 *
 * Foundation classes are CLUSTERS, and their method counts look alarming until you read where the
 * methods live: NSArray implements 127, but its concrete classes implement almost none -- AGArrayEmpty
 * 8, AGArrayItems 1, AGArrayMutable 0 -- BECAUSE THE OTHER ~110 RIDE ON THE PRIMITIVES. A Core
 * Foundation-owned class therefore needs ONLY THE PRIMITIVES and inherits the derived ones, which is the
 * arrangement Apple relies on. The real cost is Core Foundation own dispatch list, not Foundation API.
 *
 * EVERY IMP CALLS CORE FOUNDATION DOOR FOR THAT METHOD, and it cannot recurse: _CFIsObjC refuses this
 * class, so the door takes its native path. THE ENCODING MUST MATCH THE METHOD IT REPLACES -- measured
 * twice as libobjc2 'incorrect signature', and a wrong one gives a silently wrong answer rather than a
 * loud failure, which is how -_secondsFromGMTForAbsoluteTime: returning NSInteger cost an afternoon.
 *
 * SELECTORS ARE NAMED, NOT @selector()d: @selector is a call to sel_registerName and cannot appear in a
 * static initialiser, so the table holds names and cf_bridged_subclass registers them.
 * ------------------------------------------------------------------------- */
typedef struct {
	const char *name;
	IMP imp;
	const char *types;
} cf_own_method;

static double cf_imp_date_absolute_time(id self, SEL _cmd)
{
	(void)_cmd;
	return (double)CFDateGetAbsoluteTime((CFDateRef)self);
}

static double cf_imp_date_since(id self, SEL _cmd, id other)
{
	(void)_cmd;
	return (double)CFDateGetTimeIntervalSinceDate((CFDateRef)self, (CFDateRef)other);
}

static long cf_imp_date_compare(id self, SEL _cmd, id other)
{
	(void)_cmd;
	return (long)CFDateCompare((CFDateRef)self, (CFDateRef)other, NULL);
}

/* NSNumber PRIMITIVES, AND IT NEEDED NO REFACTOR -- unlike NSDate, whose methods read the ivar
 * _timeIntervalSince1970 and could therefore inherit nothing. base NSNumber has ZERO ivar mentions in
 * the abstract class and five concrete AGNumber* classes each implementing the same four methods, which
 * IS the cluster shape. So this is a one-step job, and the four are exactly the concrete set:
 *
 *   -agKind                 the @encode char of the type it was built as, WHICH IS THE DISCRIMINATOR
 *                           every conversion in the abstract class switches on: unsigned kinds take
 *                           -unsignedLongLongValue, 'f' and 'd' take -doubleValue, the rest
 *                           -longLongValue. Get this wrong and fifteen conversions are wrong.
 *   -longLongValue
 *   -unsignedLongLongValue
 *   -doubleValue
 */
static unsigned char cf_imp_number_kind(id self, SEL _cmd)
{
	(void)_cmd;
	switch (CFNumberGetType((CFNumberRef)self)) {
	case kCFNumberSInt8Type:	return (unsigned char)'c';
	case kCFNumberSInt16Type:	return (unsigned char)'s';
	case kCFNumberSInt32Type:	return (unsigned char)'i';
	case kCFNumberSInt64Type:	return (unsigned char)'q';
	case kCFNumberFloat32Type:	return (unsigned char)'f';
	case kCFNumberFloat64Type:	return (unsigned char)'d';
	case kCFNumberCharType:		return (unsigned char)'C';
	case kCFNumberShortType:	return (unsigned char)'S';
	case kCFNumberIntType:		return (unsigned char)'I';
	case kCFNumberLongType:		return (unsigned char)'L';
	case kCFNumberLongLongType:	return (unsigned char)'Q';
	case kCFNumberFloatType:	return (unsigned char)'f';
	case kCFNumberDoubleType:	return (unsigned char)'d';
	default:			return (unsigned char)'q';
	}
}

static long long cf_imp_number_ll(id self, SEL _cmd)
{
	long long v = 0;

	(void)_cmd;
	CFNumberGetValue((CFNumberRef)self, kCFNumberSInt64Type, &v);
	return v;
}

static unsigned long long cf_imp_number_ull(id self, SEL _cmd)
{
	long long v = 0;

	(void)_cmd;
	CFNumberGetValue((CFNumberRef)self, kCFNumberSInt64Type, &v);
	return (unsigned long long)v;
}

static double cf_imp_number_double(id self, SEL _cmd)
{
	double v = 0.0;

	(void)_cmd;
	CFNumberGetValue((CFNumberRef)self, kCFNumberFloat64Type, &v);
	return v;
}

/* NSBOOLEAN'S PRIMITIVES, AND THEY ARE THE SAME FOUR THE NUMBER ROW HAS -- because the class being built
 * is Foundation's NSNumber machinery reading Core Foundation's storage, and that machinery asks -agKind and
 * a value door and derives everything else (-boolValue, -objCType, -description, -isEqual:) from them.
 *
 * THE KIND IS 'C' AND NOT 'c', WHICH IS THIS TREE'S @encode(BOOL) AND NOT APPLE'S. Apple's -objCType for
 * @YES is "c"; here it is "C", because this Foundation's BOOL is an unsigned char and its -agKind has
 * always been the @encode of the type the number was made with. Matching the kind the AGNumberBoolean class
 * already uses is what keeps [@YES isEqual:kCFBooleanTrue] true; changing it would be a separate decision
 * about @encode(BOOL) across the whole library. */
static unsigned char cf_imp_boolean_kind(id self, SEL _cmd)
{
	(void)_cmd; (void)self;
	/* 'c' AND NOT 'C', which is Apple's @encode(BOOL) and what Foundation's Boolean NUMBER answers from the
	 * other side of the bridge (AGNumberBoolean's -agKind in libagora-base). Matching it here is what keeps
	 * [@YES isEqual:kCFBooleanTrue] true; the comment in the immutable BOOLEAN row above records the rest. */
	return 'c';
}

static long long cf_imp_boolean_ll(id self, SEL _cmd)
{
	(void)_cmd;
	return ((CFBooleanRef)self == kCFBooleanTrue) ? 1 : 0;
}

static unsigned long long cf_imp_boolean_ull(id self, SEL _cmd)
{
	(void)_cmd;
	return ((CFBooleanRef)self == kCFBooleanTrue) ? 1 : 0;
}

static double cf_imp_boolean_double(id self, SEL _cmd)
{
	(void)_cmd;
	return ((CFBooleanRef)self == kCFBooleanTrue) ? 1.0 : 0.0;
}

static const cf_own_method cf_boolean_methods[] = {
	{ "agKind", (IMP)cf_imp_boolean_kind, "C@:" },
	{ "longLongValue", (IMP)cf_imp_boolean_ll, "q@:" },
	{ "unsignedLongLongValue", (IMP)cf_imp_boolean_ull, "Q@:" },
	{ "doubleValue", (IMP)cf_imp_boolean_double, "d@:" },
	{ NULL, NULL, NULL },
};

static const cf_own_method cf_number_methods[] = {
	{ "agKind", (IMP)cf_imp_number_kind, "C@:" },
	{ "longLongValue", (IMP)cf_imp_number_ll, "q@:" },
	{ "unsignedLongLongValue", (IMP)cf_imp_number_ull, "Q@:" },
	{ "doubleValue", (IMP)cf_imp_number_double, "d@:" },
	{ NULL, NULL, NULL },
};

/* NSArray PRIMITIVES, AND THEY ARE EXACTLY TWO. Every concrete AGArray* class implements -count and
 * -objectAtIndex: and nothing else of the collection surface -- AGArrayEmpty, AGArrayOne and
 * AGArraySmall each define those two and inherit the other ~125 methods of NSArray. That is the cluster
 * shape, so this is a one-step job.
 *
 * THE MUTABLE CASE IS NOT COVERED AND CANNOT BE FROM HERE: CFArrayRef and CFMutableArrayRef share ONE
 * type id, so the owned class cannot tell them apart, and a CFMutableArrayRef would answer -addObject:
 * with an inherited NSMutableArray implementation reading Core Foundation storage. Apple has
 * __NSCFArray and __NSCFMutableArray and picks between them at CREATION -- which is what the `category`
 * parameter of _CFRuntimeCreateInstance exists for, and CFArray.c passes NULL. */
static unsigned long cf_imp_array_count(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFArrayGetCount((CFArrayRef)self);
}

static id cf_imp_array_object_at(id self, SEL _cmd, unsigned long idx)
{
	(void)_cmd;
	return (id)CFArrayGetValueAtIndex((CFArrayRef)self, (CFIndex)idx);
}

static const cf_own_method cf_array_methods[] = {
	{ "count", (IMP)cf_imp_array_count, "Q@:" },
	{ "objectAtIndex:", (IMP)cf_imp_array_object_at, "@@:Q" },
	{ NULL, NULL, NULL },
};

/* ---------------------------------------------------------------------------------------------------- */
/* THE MUTABLE HALF: ONE TYPE ID, A SECOND CLASS, AND THE STORAGE OPERATIONS AS THE PRIMITIVES           */
/* ---------------------------------------------------------------------------------------------------- */
/* CFMutableArrayRef and CFArrayRef SHARE ONE TYPE ID, so nothing about an object says which it is. Apple
 * picks the class at CREATION and so does this, from the category _CFRuntimeCreateInstance already takes.
 *
 * THE PRIMITIVE SET IS FOUNDATION'S SEVEN, the same seven libagora-base's NSMutableArray was routed through
 * so a class with storage of its own can answer the rest: -count, -objectAtIndex:, -insertObject:atIndex:,
 * -removeObjectAtIndex:, -removeAllObjects, -replaceObjectAtIndex:withObject: and -fnMutationWord. The last
 * is why the accessor in CFArray.c exists: a fast-enumeration loop polls an ADDRESS, and it has to be Core
 * Foundation's counter.
 *
 * -initWithCapacity: IS A NO-OP ON PURPOSE: a CFArrayCreateMutable object is already built, and inheriting
 * Foundation's initialiser would allocate FOUNDATION's storage and write its ivars into this object.
 * Anything not on this list must not be inherited from NSMutableArray either -- the list is the contract.
 *
 * AND THE TYPES OF THESE METHODS COME FROM FOUNDATION AND NOT FROM HERE: see the note above
 * cf_bridged_subclass. An abbreviated encoding gets a two-argument selector NO SEND AT ALL. */
static void cf_imp_mutable_array_insert(id self, SEL _cmd, id object, unsigned long index)
{
	(void)_cmd;
	CFArrayInsertValueAtIndex((CFMutableArrayRef)self, (CFIndex)index, (const void *)object);
}

static void cf_imp_mutable_array_remove(id self, SEL _cmd, unsigned long index)
{
	(void)_cmd;
	CFArrayRemoveValueAtIndex((CFMutableArrayRef)self, (CFIndex)index);
}

static void cf_imp_mutable_array_remove_all(id self, SEL _cmd)
{
	(void)_cmd;
	CFArrayRemoveAllValues((CFMutableArrayRef)self);
}

static void cf_imp_mutable_array_replace(id self, SEL _cmd, unsigned long index, id object)
{
	(void)_cmd;
	CFArraySetValueAtIndex((CFMutableArrayRef)self, (CFIndex)index, (const void *)object);
}

static unsigned long *cf_imp_mutable_array_mutation_word(id self, SEL _cmd)
{
	(void)_cmd;
	return __CFArrayGetMutationWord((CFArrayRef)self);
}

static id cf_imp_mutable_array_init_with_capacity(id self, SEL _cmd, unsigned long capacity)
{
	(void)_cmd; (void)capacity;
	return self;
}

/* The types below are FALLBACKS. What is registered is the encoding of FOUNDATION'S OWN method for the
 * selector, found on the superclass when the class is built; these strings are reached only if Foundation
 * has no such method at all. */
static const cf_own_method cf_mutable_array_methods[] = {
	{ "count", (IMP)cf_imp_array_count, "Q@:" },
	{ "objectAtIndex:", (IMP)cf_imp_array_object_at, "@@:Q" },
	{ "insertObject:atIndex:", (IMP)cf_imp_mutable_array_insert, "v@:@@Q" },
	{ "removeObjectAtIndex:", (IMP)cf_imp_mutable_array_remove, "v@:Q" },
	{ "removeAllObjects", (IMP)cf_imp_mutable_array_remove_all, "v@:" },
	{ "replaceObjectAtIndex:withObject:", (IMP)cf_imp_mutable_array_replace, "v@:Q@@" },
	{ "fnMutationWord", (IMP)cf_imp_mutable_array_mutation_word, "^Q@:" },
	{ "initWithCapacity:", (IMP)cf_imp_mutable_array_init_with_capacity, "@@:Q" },
	{ NULL, NULL, NULL },
};

/* THE TYPES THAT HAVE A MUTABLE HALF, AND WHAT EACH ONE IS A KIND OF. The superclass is FOUNDATION'S,
 * named by string and found with objc_getClass when the owned class is built -- so a Foundation that
 * renamed or dropped one gets NO mutable class, and the loud raise that comes with it, rather than a class
 * built on the wrong parent. */


/* NSData PRIMITIVES, AND THEY ARE THREE. AGDataItems, AGDataEmpty and AGDataMutable implement NO methods
 * of the byte surface between them -- the front's OWN ivars are the storage, so -length and -bytes are
 * written once on NSData over `_length` and `_bytes` -- which makes this the FIRST type whose primitives
 * are NOT the concrete classes' but the base class's. They are still only the methods that READ THAT
 * STORAGE: -length, -bytes, and -getBytes: (the legacy void form, which memcpy's from `_bytes` itself).
 * -getBytes:length: and -getBytes:range: are already written over -length and -bytes, so they come free.
 *
 * THE MUTABLE CASE IS NOT COVERED, for the reason CFArray's note gives -- CFDataRef and CFMutableDataRef
 * share one type id. */
static unsigned long cf_imp_data_length(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFDataGetLength((CFDataRef)self);
}

static const void *cf_imp_data_bytes(id self, SEL _cmd)
{
	(void)_cmd;
	return (const void *)CFDataGetBytePtr((CFDataRef)self);
}

static void cf_imp_data_get_bytes(id self, SEL _cmd, void *buffer)
{
	(void)_cmd;
	if (buffer != NULL) {
		CFDataGetBytes((CFDataRef)self, CFRangeMake(0, CFDataGetLength((CFDataRef)self)), (UInt8 *)buffer);
	}
}

static const cf_own_method cf_data_methods[] = {
	{ "length", (IMP)cf_imp_data_length, "Q@:" },
	{ "bytes", (IMP)cf_imp_data_bytes, "r^v@:" },
	{ "getBytes:", (IMP)cf_imp_data_get_bytes, "v@:^v" },
	{ NULL, NULL, NULL },
};

/* ---------------------------------------------------------------------------------------------------- */
/* THE MUTABLE HALF OF AN NSData: THE METHODS THAT WRITE FOUNDATION'S STORAGE                            */
/* ---------------------------------------------------------------------------------------------------- */
/* SEVEN, AND THEY WERE FOUND BY READING THE CLASS RATHER THAN BY TRUSTING ITS DOCUMENTATION -- the lesson
 * the mutable string taught at the cost of twenty-one suite cases. libagora-base's NSData.h names the two
 * buffer accessors (-bytes and -mutableBytes); MEASURED, the mutable class writes storage in exactly these
 * seven: -mutableBytes, -setLength:, -appendBytes:length:, -replaceBytesInRange:withBytes:length:,
 * -resetBytesInRange:, -increaseLengthBy: and -setData: (the last reads _length, which is why the cap of
 * twenty-four lines I first measured with HID it). The three read doors are the immutable class's own.
 *
 * EVERY ONE IS ANSWERED, NOT DERIVED, because a method here that Foundation's implementation still runs
 * writes ITS storage into a Core Foundation object -- the silent corruption this bridge has met. */
static unsigned char *cf_imp_mutable_data_mutable_bytes(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned char *)CFDataGetMutableBytePtr((CFMutableDataRef)self);
}

static void cf_imp_mutable_data_set_length(id self, SEL _cmd, unsigned long length)
{
	(void)_cmd;
	CFDataSetLength((CFMutableDataRef)self, (CFIndex)length);
}

static void cf_imp_mutable_data_append(id self, SEL _cmd, const void *bytes, unsigned long length)
{
	(void)_cmd;
	CFDataAppendBytes((CFMutableDataRef)self, (const UInt8 *)bytes, (CFIndex)length);
}

static void cf_imp_mutable_data_replace(id self, SEL _cmd, NSRange range, const void *bytes, unsigned long length)
{
	(void)_cmd;
	CFDataReplaceBytes((CFMutableDataRef)self, CFRangeMake((CFIndex)range.location, (CFIndex)range.length),
			   (const UInt8 *)bytes, (CFIndex)length);
}

static void cf_imp_mutable_data_reset(id self, SEL _cmd, NSRange range)
{
	unsigned char *buffer;
	CFIndex length;

	(void)_cmd;
	buffer = (unsigned char *)CFDataGetMutableBytePtr((CFMutableDataRef)self);
	length = CFDataGetLength((CFDataRef)self);
	if (buffer == NULL || (CFIndex)(range.location + range.length) > length) {
		return;
	}
	memset(buffer + range.location, 0, range.length);
}

static void cf_imp_mutable_data_increase(id self, SEL _cmd, unsigned long extra)
{
	(void)_cmd;
	CFDataSetLength((CFMutableDataRef)self, CFDataGetLength((CFDataRef)self) + (CFIndex)extra);
}

static void cf_imp_mutable_data_set_data(id self, SEL _cmd, id other)
{
	CFIndex otherLength;

	(void)_cmd;
	otherLength = (other != nil) ? CFDataGetLength((CFDataRef)other) : 0;
	CFDataReplaceBytes((CFMutableDataRef)self, CFRangeMake(0, CFDataGetLength((CFDataRef)self)),
			   (other != nil) ? CFDataGetBytePtr((CFDataRef)other) : NULL, otherLength);
	CFDataSetLength((CFMutableDataRef)self, otherLength);
}

static id cf_imp_mutable_data_init_with_capacity(id self, SEL _cmd, unsigned long capacity)
{
	(void)_cmd; (void)capacity;
	return self;
}

static const cf_own_method cf_mutable_data_methods[] = {
	{ "length", (IMP)cf_imp_data_length, "Q@:" },
	{ "bytes", (IMP)cf_imp_data_bytes, "r^v@:" },
	{ "getBytes:", (IMP)cf_imp_data_get_bytes, "v@:^v" },
	{ "mutableBytes", (IMP)cf_imp_mutable_data_mutable_bytes, "^v@:" },
	{ "setLength:", (IMP)cf_imp_mutable_data_set_length, "v@:Q" },
	{ "appendBytes:length:", (IMP)cf_imp_mutable_data_append, "v@:^vQ" },
	{ "replaceBytesInRange:withBytes:length:", (IMP)cf_imp_mutable_data_replace, "v@:{_NSRange=QQ}^vQ" },
	{ "resetBytesInRange:", (IMP)cf_imp_mutable_data_reset, "v@:{_NSRange=QQ}" },
	{ "increaseLengthBy:", (IMP)cf_imp_mutable_data_increase, "v@:Q" },
	{ "setData:", (IMP)cf_imp_mutable_data_set_data, "v@:@" },
	{ "init", (IMP)cf_imp_mutable_data_init_with_capacity, "@@:" },
	{ "initWithCapacity:", (IMP)cf_imp_mutable_data_init_with_capacity, "@@:Q" },
	{ "initWithLength:", (IMP)cf_imp_mutable_data_init_with_capacity, "@@:Q" },
	{ NULL, NULL, NULL },
};

/* ---------------------------------------------------------------------------
 * THE TWO CONTAINER TYPES, AND THE ONE PRIMITIVE CORE FOUNDATION HAS NO DOOR FOR.
 *
 * NSDictionary's primitives are -count, -objectForKey: and -keyEnumerator; NSSet's are -count, -member:
 * and -objectEnumerator. FIVE OF THE SIX ARE CORE FOUNDATION DOORS AND THE SIXTH IS NOT, and it is not a
 * gap in Core Foundation: Core Foundation HAS no enumerator object, which is why an enumerator is a
 * Foundation class. So the six is answered from THE OTHER SIDE OF THE SAME BRIDGE -- a KEY SNAPSHOT is
 * built with CFArray (pure Core Foundation) and the enumerator is asked OF that array as an Objective-C
 * object. The array only has to be an NSArray for the length of one message, and it is one: CFArray's
 * type id is bridged to NSArray and CF's own arrays now wear __NSCFArray, so -objectEnumerator is
 * NSArray's implementation counting through CFArrayGetCount/GetValueAtIndex.
 *
 * NOTHING LEAKS: the enumerator's own -initWithSequence: takes a FOUNDATION COPY of the sequence (and
 * retains each element), so the Core Foundation snapshot is released as soon as the message returns.
 * ------------------------------------------------------------------------- */
extern id objc_msgSend(id self, SEL _cmd, ...);	/* libobjc2 declares it in objc/message.h, not here */
extern id objc_autorelease(id obj);		/* ... and this one in objc/objc-arc.h */
extern void *objc_autoreleasePoolPush(void);	/* ... and these two, for the reason -autorelease gives */
extern void objc_autoreleasePoolPop(void *pool);

static id cf_imp_snapshot_enumerator(const void **values, CFIndex count)
{
	CFArrayRef snapshot = CFArrayCreate(kCFAllocatorDefault, values, count, &kCFTypeArrayCallBacks);
	id enumerator;

	if (snapshot == NULL) {
		return NULL;
	}
	enumerator = objc_msgSend((id)snapshot, sel_registerName("objectEnumerator"));
	CFRelease(snapshot);
	return enumerator;
}

static unsigned long cf_imp_dictionary_count(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFDictionaryGetCount((CFDictionaryRef)self);
}

static id cf_imp_dictionary_object_for_key(id self, SEL _cmd, id key)
{
	(void)_cmd;
	return (id)CFDictionaryGetValue((CFDictionaryRef)self, (const void *)key);
}

static id cf_imp_dictionary_key_enumerator(id self, SEL _cmd)
{
	CFIndex count = CFDictionaryGetCount((CFDictionaryRef)self);
	const void **keys = NULL;
	id enumerator;

	(void)_cmd;
	if (count > 0) {
		keys = (const void **)malloc((size_t)count * sizeof(void *));
		if (keys == NULL) {
			return NULL;
		}
		CFDictionaryGetKeysAndValues((CFDictionaryRef)self, keys, NULL);
	}
	enumerator = cf_imp_snapshot_enumerator(keys, count);
	free(keys);
	return enumerator;
}

static const cf_own_method cf_dictionary_methods[] = {
	{ "count", (IMP)cf_imp_dictionary_count, "Q@:" },
	{ "objectForKey:", (IMP)cf_imp_dictionary_object_for_key, "@@:@" },
	{ "keyEnumerator", (IMP)cf_imp_dictionary_key_enumerator, "@@:" },
	{ NULL, NULL, NULL },
};

/* NSDictionary's MUTABLE PRIMITIVES, AND THE THREE THAT MATTER ARE FOUNDATION'S OWN INTERNAL DOORS --
 * -setObjectInternal:forKey:, -removeObjectInternalForKey: and -removeAllInternalObjects -- which are where
 * ITS storage operations live and therefore exactly what a class with storage elsewhere must answer. The
 * first three entries are the immutable class's, since a mutable dictionary must also read. */
static void cf_imp_mutable_dictionary_set(id self, SEL _cmd, id value, id key)
{
	(void)_cmd;
	CFDictionarySetValue((CFMutableDictionaryRef)self, (const void *)key, (const void *)value);
}

static void cf_imp_mutable_dictionary_remove(id self, SEL _cmd, id key)
{
	(void)_cmd;
	CFDictionaryRemoveValue((CFMutableDictionaryRef)self, (const void *)key);
}

static void cf_imp_mutable_dictionary_remove_all(id self, SEL _cmd)
{
	(void)_cmd;
	CFDictionaryRemoveAllValues((CFMutableDictionaryRef)self);
}

static unsigned long *cf_imp_mutable_dictionary_mutation_word(id self, SEL _cmd)
{
	(void)_cmd;
	return __CFBasicHashGetMutationWord((void *)self);
}

static id cf_imp_mutable_dictionary_init(id self, SEL _cmd)
{
	(void)_cmd;
	return self;
}

static const cf_own_method cf_mutable_dictionary_methods[] = {
	{ "count", (IMP)cf_imp_dictionary_count, "Q@:" },
	{ "objectForKey:", (IMP)cf_imp_dictionary_object_for_key, "@@:@" },
	{ "keyEnumerator", (IMP)cf_imp_dictionary_key_enumerator, "@@:" },
	{ "setObjectInternal:forKey:", (IMP)cf_imp_mutable_dictionary_set, "v@:@@" },
	{ "removeObjectInternalForKey:", (IMP)cf_imp_mutable_dictionary_remove, "v@:@" },
	{ "removeAllInternalObjects", (IMP)cf_imp_mutable_dictionary_remove_all, "v@:" },
	{ "fnMutationWord", (IMP)cf_imp_mutable_dictionary_mutation_word, "^Q@:" },
	{ "init", (IMP)cf_imp_mutable_dictionary_init, "@@:" },
	{ "initWithCapacity:", (IMP)cf_imp_mutable_dictionary_init, "@@:Q" },
	{ NULL, NULL, NULL },
};



static unsigned long cf_imp_set_count(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFSetGetCount((CFSetRef)self);
}

static id cf_imp_set_member(id self, SEL _cmd, id object)
{
	(void)_cmd;
	return object == nil ? nil : (id)CFSetGetValue((CFSetRef)self, (const void *)object);
}

static id cf_imp_set_object_enumerator(id self, SEL _cmd)
{
	CFIndex count = CFSetGetCount((CFSetRef)self);
	const void **values = NULL;
	id enumerator;

	(void)_cmd;
	if (count > 0) {
		values = (const void **)malloc((size_t)count * sizeof(void *));
		if (values == NULL) {
			return NULL;
		}
		CFSetGetValues((CFSetRef)self, values);
	}
	enumerator = cf_imp_snapshot_enumerator(values, count);
	free(values);
	return enumerator;
}

static const cf_own_method cf_set_methods[] = {
	{ "count", (IMP)cf_imp_set_count, "Q@:" },
	{ "member:", (IMP)cf_imp_set_member, "@@:@" },
	{ "objectEnumerator", (IMP)cf_imp_set_object_enumerator, "@@:" },
	{ NULL, NULL, NULL },
};

/* ---------------------------------------------------------------------------------------------------- */
/* THE MUTABLE HALF OF A SET: ONE STORAGE DOOR, AND THE SAME CFBasicHash A DICTIONARY IS                */
/* ---------------------------------------------------------------------------------------------------- */
/* A set's whole storage contract is ONE door -- -fnReplaceMembers: -- and MEASURED, that is what every
 * mutable operation in Foundation funnels through: -addObject: copies the members, appends, and replaces;
 * -unionSet:, -minusSet:, -intersectSet: and -setSet: compute the new membership and replace. So a class
 * with storage of its own answers that one door and inherits a complete mutable set -- the cheapest of
 * these types so far, and the first that needed NO Foundation routing at all beyond its mutation word.
 *
 * A CFSet IS A CFBasicHash, exactly like a CFDictionary, so its mutable creator re-stamps the object and
 * the CATEGORY is what says which half it is. */
static void cf_imp_mutable_set_replace(id self, SEL _cmd, id members)
{
	NSUInteger i, n;

	(void)_cmd;
	/* REPLACE MEANS EXACTLY THESE, so the set is emptied first: what the door promises is the membership
	 * it is handed, not a union with what was there. */
	CFSetRemoveAllValues((CFMutableSetRef)self);
	n = (members != nil) ? (NSUInteger)[members count] : 0;
	for (i = 0; i < n; i++) {
		CFSetAddValue((CFMutableSetRef)self, (const void *)[members objectAtIndex:i]);
	}
}

static unsigned long *cf_imp_mutable_set_mutation_word(id self, SEL _cmd)
{
	(void)_cmd;
	return __CFBasicHashGetMutationWord((void *)self);
}

static id cf_imp_mutable_set_init(id self, SEL _cmd)
{
	(void)_cmd;
	return self;
}

static const cf_own_method cf_mutable_set_methods[] = {
	{ "count", (IMP)cf_imp_set_count, "Q@:" },
	{ "member:", (IMP)cf_imp_set_member, "@@:@" },
	{ "objectEnumerator", (IMP)cf_imp_set_object_enumerator, "@@:" },
	{ "fnReplaceMembers:", (IMP)cf_imp_mutable_set_replace, "v@:@@" },
	{ "fnMutationWord", (IMP)cf_imp_mutable_set_mutation_word, "^Q@:" },
	{ "init", (IMP)cf_imp_mutable_set_init, "@@:" },
	{ "initWithCapacity:", (IMP)cf_imp_mutable_set_init, "@@:Q" },
	{ NULL, NULL, NULL },
};

static const cf_own_method cf_string_methods[] = {
	{ "length", (IMP)cf_imp_string_length, "Q@:" },
	{ "characterAtIndex:", (IMP)cf_imp_string_char_at, "S@:Q" },
	{ "UTF8String", (IMP)cf_imp_string_utf8, "r*@:" },
	{ NULL, NULL, NULL },
};

/* ---------------------------------------------------------------------------------------------------- */
/* THE MUTABLE HALF OF A STRING: THE TWO DOORS THAT WRITE FOUNDATION'S STORAGE                          */
/* ---------------------------------------------------------------------------------------------------- */
/* libagora-base's NSString.h DOCUMENTS the contract this rests on: "-byteAtIndex: defaults to a read of
 * -UTF8String, so a subclass that answers -length, -characterAtIndex: and -UTF8String is correct through
 * the derived doors without any storage of its own". So the three read doors are the immutable class's
 * own and are listed here too, and a mutable string adds the two methods of Foundation's mutable string
 * that WRITE storage: MEASURED, -setString: (8 storage writes) and -appendUTF8String: (14). Everything
 * else is derived from one of them -- -appendString: calls -appendUTF8String:, and
 * -replaceCharactersInRange:withString: builds a string and calls -setString:. */
static void cf_imp_mutable_string_set(id self, SEL _cmd, id string)
{
	(void)_cmd;
	CFStringReplace((CFMutableStringRef)self,
			CFRangeMake(0, CFStringGetLength((CFStringRef)self)), (CFStringRef)string);
}

static void cf_imp_mutable_string_append_utf8(id self, SEL _cmd, const char *utf8)
{
	CFStringRef built;

	(void)_cmd;
	if (utf8 == NULL || *utf8 == '\0') {
		return;
	}
	built = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
	if (built != NULL) {
		CFStringAppend((CFMutableStringRef)self, built);
		CFRelease(built);
	}
}

static id cf_imp_mutable_string_init_with_capacity(id self, SEL _cmd, unsigned long capacity)
{
	(void)_cmd; (void)capacity;
	return self;
}

static const cf_own_method cf_mutable_string_methods[] = {
	{ "length", (IMP)cf_imp_string_length, "Q@:" },
	{ "characterAtIndex:", (IMP)cf_imp_string_char_at, "S@:Q" },
	{ "UTF8String", (IMP)cf_imp_string_utf8, "r*@:" },
	{ "setString:", (IMP)cf_imp_mutable_string_set, "v@:@" },
	{ "appendUTF8String:", (IMP)cf_imp_mutable_string_append_utf8, "v@:*" },
	{ "init", (IMP)cf_imp_mutable_string_init_with_capacity, "@@:" },
	{ "initWithCapacity:", (IMP)cf_imp_mutable_string_init_with_capacity, "@@:Q" },
	{ NULL, NULL, NULL },
};

static const cf_own_method cf_date_methods[] = {
	{ "timeIntervalSinceReferenceDate", (IMP)cf_imp_date_absolute_time, "d@:" },
	{ "timeIntervalSinceDate:", (IMP)cf_imp_date_since, "d@:@" },
	{ "compare:", (IMP)cf_imp_date_compare, "i@:@" },
	{ NULL, NULL, NULL },
};

/* EVERY ROW GETS THESE, BECAUSE THE REFERENCE COUNT IS THE ONE THING EVERY CORE FOUNDATION OBJECT HAS IN
 * COMMON: it is Core Foundation's own, in cfinfoa, and the Objective-C retain family has to answer with
 * THAT count rather than with one of its own.
 *
 * THE OTHER HALF OF THE SAME MEASUREMENT as CFRetain's note. libobjc2 keeps its retain count at
 * ((uintptr_t *)obj)[-1] -- for a CFRuntime allocation, the malloc chunk header -- and a probe showed the
 * OBJECTIVE-C side moving it just as CFRetain used to: `[aNativeCFData retain]` read 0x51 and left 0x52,
 * `-release` 0x51 again. Two counts for one object, and neither of them Core Foundation's.
 *
 * SO THE MESSAGE GOES BACK TO THE C API, which is the same answer Apple gives: objc_retain on a
 * CF-created object IS CFRetain on it.
 *
 * AND -autorelease DEFERS, RATHER THAN ANSWERING THE IDENTITY. It answers CFAutorelease, which in this
 * tree is a no-op stub -- so `[anObjectIJustCreated autorelease]` on a Core Foundation object dropped the
 * release on the floor, and a method whose contract is "+0" (every -Copy- and -Create-shaped accessor in
 * the door table below) had no way to answer without leaking. This IMP puts the object in the runtime's
 * autorelease pool instead, and the count still ends up in the right place: the pool drains with
 * objc_release, which for a class that DEFINES -release is a message to -release, which is the method
 * three lines up.
 *
 * AND IT PRIMES THE RUNTIME'S POOL FLAG FIRST, WHICH IS NOT SUPERSTITION -- IT IS THE ONLY WAY OUT OF A
 * LOOP. libobjc2's autorelease() asks three questions in order: `useARCAutoreleasePool`, then
 * `objc_class_flag_fast_arc`, and if both are no IT SENDS -autorelease TO THE OBJECT. Our classes have
 * neither flag (the second one is exactly what would send retain and release back to the malloc chunk
 * header -- see the note above), so calling objc_autorelease from this method is a message back to this
 * method: measured, as a stack that ran away. objc_autoreleasePoolPush() is what sets
 * useARCAutoreleasePool (libobjc2's initAutorelease, which base's NSAutoreleasePool answers with
 * -_ARCCompatibleAutoreleasePool), so this pushes and pops one empty pool ONCE to prime it and then
 * objc_autorelease takes the pool path it was always meant to take.
 *
 * AND base's +[NSAutoreleasePool addObject:] IS NOT THE WAY OUT, though it looks like one: its body IS
 * `[object autorelease]`, so asking the pool to add the object lands back in this method. It was written
 * down here as the way out before it was read.
 *
 * THE IMP CANNOT RECURSE, OTHERWISE: CFRetain asks _CFIsOwnedCFObject first (see its note) and takes the
 * Core Foundation path for exactly these classes, so the door never sends -retain back to this method. */
static id cf_imp_object_retain(id self, SEL _cmd)
{
	(void)_cmd;
	return (id)CFRetain((CFTypeRef)self);
}

static void cf_imp_object_release(id self, SEL _cmd)
{
	(void)_cmd;
	CFRelease((CFTypeRef)self);
}

static Boolean cf_object_autorelease_pool_ready = false;

static id cf_imp_object_autorelease(id self, SEL _cmd)
{
	(void)_cmd;
	if (!cf_object_autorelease_pool_ready) {
		void *token = objc_autoreleasePoolPush();

		objc_autoreleasePoolPop(token);	/* empty, and that is the point: it runs initAutorelease */
		cf_object_autorelease_pool_ready = true;
	}
	return objc_autorelease((id)self);
}

static const cf_own_method cf_object_methods[] = {
	{ "retain", (IMP)cf_imp_object_retain, "@@:" },
	{ "release", (IMP)cf_imp_object_release, "v@:" },
	{ "autorelease", (IMP)cf_imp_object_autorelease, "@@:" },
	{ NULL, NULL, NULL },
};

/* One row per type that has been given an owned class. CFDate is here because it is the cheapest real
 * one at three methods; ONE REGISTERED TYPE STILL MISSING FROM THIS TABLE WEARS NO CLASS, which is why
 * handing one to Foundation segfaults and why the suite says so out loud:
 *
 *   CFURL -- NOT A CLUSTER. Its accessors read the base class's OWN ivars, so a subclass that implements a
 *   few primitives inherits a hundred methods that read garbage. It needs the treatment NSDate got: make
 *   the storage readers the abstract primitives on the Foundation side first.
 *
 * CFTimeZone IS IN THE TABLE AND IS VERIFIED, AND IT TOOK THREE FIXES ON THE WAY IN -- all three in
 * libagora-base, all three reached only because CFTimeZoneCreateWithName builds its zone path from a
 * CFSTR and Core Foundation's file-URL construction then walks into Foundation:
 *
 *   NSCFConstantString's two encoding doors CALLED THE DOOR THAT DISPATCHES TO THEM (a stack overflow
 *   523,844 frames deep, and the reason CFTimeZoneCreateWithName died for every argument);
 *   the same class did not implement -_getCString:maxLength:encoding: at all, which is the door
 *   CFStringGetCString dispatches to;
 *   and that new implementation CAST a CFStringEncoding to an NSStringEncoding, on the wrong assumption
 *   that the two enumerations agree -- they do not (kCFStringEncodingUTF8 is 0x08000100, NSUTF8StringEncoding
 *   is 4), so CFStringGetCString answered false, POSIXPathToURLPath answered NULL, and
 *   CFURLCreateCopyAppendingPathComponent dereferenced that NULL.
 *
 * AND ONE ROW HAS NO METHODS AT ALL. CFNull is a NAME and nothing else: NSNull holds no storage, and its
 * -description, -isEqual: (identity) and -hash (a constant) are content-free, so there is nothing for a
 * subclass to re-answer. THE ROW STILL MATTERS, because the class has to EXIST before the only CFNull
 * there is can wear it -- and that instance is a static whose isa is a compile-time NULL, which
 * cf_bridged_subclass writes in through __CFNullBindOwnedClass the moment this class is made. */
static const cf_own_method cf_null_methods[] = {
	{ NULL, NULL, NULL },
};

/* NSTimeZone PRIMITIVES, AND THERE ARE EIGHT -- THE MOST ANY TYPE HAS NEEDED, AND IT IS STILL NOT A
 * REIMPLEMENTATION. They are the eight methods on NSTimeZone that READ THE BASE CLASS'S OWN IVARS
 * (`_name`, `_secondsFromGMT`, `_data`); everything else on the class is already written over them --
 * -isEqualToTimeZone:, -hash and -description over -name; -isDaylightSavingTime and
 * -nextDaylightSavingTimeTransition over their ...ForDate: forms; and -secondsFromGMT over
 * -secondsFromGMTForDate:, which is the ONE line of Foundation this needed (NSTimeZone.m). That is the
 * treatment NSDate got, and the reason it is needed at all: NSTimeZone is not a class cluster, so its
 * concrete class IS the class with the storage.
 *
 * THE DATES CROSS AS ABSOLUTE TIMES, WHICH IS THE SAME CONVERSION THE OTHER DIRECTION ALREADY MAKES.
 * Every Core Foundation time zone door takes a CFAbsoluteTime and Foundation hands these methods an
 * NSDate, so each one sends -timeIntervalSinceReferenceDate -- the primitive the CFDate owned class
 * answers from CFDateGetAbsoluteTime.
 *
 * THREE OF THEM ANSWER A +0 METHOD FROM A +1 CORE FOUNDATION DOOR, which is why -autorelease above had to
 * be made to mean something: -abbreviationForDate: and -localizedName:locale: are Copy-shaped (+1), and
 * -nextDaylightSavingTimeTransitionAfterDate: builds its date with CFDateCreate (an object that IS an
 * NSDate, since CFDate and NSDate are one object through this bridge). All three end in `[autorelease]`,
 * and that now defers to the runtime's pool the way Foundation's own methods do.
 *
 * NONE OF THE THREE IS REACHED BY THE SUITE, which is worth writing down rather than leaving implied: it
 * drives -name, and the leak these three would otherwise have had is fixed by the retain family above
 * rather than by a test that can see it. */
static CFAbsoluteTime cf_absolute_time_of(id date)
{
	typedef double (*cf_absolute_time_fn)(id, SEL);

	return date == nil ? 0.0
			   : ((cf_absolute_time_fn)objc_msgSend)(date, sel_registerName("timeIntervalSinceReferenceDate"));
}

static id cf_imp_timezone_name(id self, SEL _cmd)
{
	(void)_cmd;
	return (id)CFTimeZoneGetName((CFTimeZoneRef)self);
}

static long cf_imp_timezone_seconds_from_gmt(id self, SEL _cmd, id date)
{
	(void)_cmd;
	return (long)CFTimeZoneGetSecondsFromGMT((CFTimeZoneRef)self, cf_absolute_time_of(date));
}

static char cf_imp_timezone_is_dst(id self, SEL _cmd, id date)
{
	(void)_cmd;
	return (char)(CFTimeZoneIsDaylightSavingTime((CFTimeZoneRef)self, cf_absolute_time_of(date)) ? 1 : 0);
}

static id cf_imp_timezone_next_transition(id self, SEL _cmd, id date)
{
	CFAbsoluteTime at;

	(void)_cmd;
	/* ZERO IS CORE FOUNDATION'S "THERE IS NO NEXT TRANSITION" (CFTimeZone.c) and nil is Foundation's,
	 * which is the same answer the base class gives a named zone with no transition table. */
	at = CFTimeZoneGetNextDaylightSavingTimeTransition((CFTimeZoneRef)self, cf_absolute_time_of(date));
	return at == 0.0 ? nil : [(id)CFDateCreate(kCFAllocatorDefault, at) autorelease];
}

static id cf_imp_timezone_abbreviation(id self, SEL _cmd, id date)
{
	(void)_cmd;
	return [(id)CFTimeZoneCopyAbbreviation((CFTimeZoneRef)self, cf_absolute_time_of(date)) autorelease];
}

static double cf_imp_timezone_dst_offset(id self, SEL _cmd, id date)
{
	(void)_cmd;
	return (double)CFTimeZoneGetDaylightSavingTimeOffset((CFTimeZoneRef)self, cf_absolute_time_of(date));
}

static id cf_imp_timezone_localized_name(id self, SEL _cmd, long style, id locale)
{
	(void)_cmd;
	/* THE TWO STYLE ENUMS ARE THE SAME SIX CASES WITH THE SAME VALUES, which is why this is a cast:
	 * NSTimeZoneNameStyleStandard is 0 in Foundation and kCFTimeZoneNameStyleStandard is 0 in Core
	 * Foundation, and so on to ShortGeneric at 5. */
	return [(id)CFTimeZoneCopyLocalizedName((CFTimeZoneRef)self, (CFTimeZoneNameStyle)style,
						(CFLocaleRef)locale) autorelease];
}

static id cf_imp_timezone_data(id self, SEL _cmd)
{
	(void)_cmd;
	/* Core Foundation's own answer, and NULL is one of them: a zone created by NAME was given no data.
	 * The base class SYNTHESIZES one for that case in a format of its own ('name|seconds'), which is
	 * Foundation's business and not Core Foundation's -- so no equivalent is repeated here. */
	return (id)CFTimeZoneGetData((CFTimeZoneRef)self);
}

static const cf_own_method cf_timezone_methods[] = {
	{ "name", (IMP)cf_imp_timezone_name, "@@:" },
	{ "secondsFromGMTForDate:", (IMP)cf_imp_timezone_seconds_from_gmt, "q@:@" },
	{ "isDaylightSavingTimeForDate:", (IMP)cf_imp_timezone_is_dst, "C@:@" },
	{ "nextDaylightSavingTimeTransitionAfterDate:", (IMP)cf_imp_timezone_next_transition, "@@:@" },
	{ "abbreviationForDate:", (IMP)cf_imp_timezone_abbreviation, "@@:@" },
	{ "daylightSavingTimeOffsetForDate:", (IMP)cf_imp_timezone_dst_offset, "d@:@" },
	{ "localizedName:locale:", (IMP)cf_imp_timezone_localized_name, "@@:q@" },
	{ "data", (IMP)cf_imp_timezone_data, "@@:" },
	{ NULL, NULL, NULL },
};

/* NSURL PRIMITIVES, AND THERE ARE TWELVE OF THEM -- the largest set any type has needed, and the reason
 * is the same one NSTimeZone had: NSURL is NOT a class cluster, so this class IS the class with the
 * storage, and its accessors read ten ivars between them. What made the list as short as twelve is the
 * other half of the job, which had to be done in libagora-base first: every method that read those ivars
 * a SECOND time now reads them through these -- the speller (fnURLWithPath:, and so the whole
 * -URLByAppendingPathComponent: family), -isEqual:, -hash, -description, -resourceSpecifier,
 * -relativePath, -parameterString, -standardizedURL, -hasDirectoryPath, -path (which decodes
 * -fnEncodedPath), and the resource-value doors.
 *
 * TWO OF THEM NEED A WORD, because they are not the Core Foundation door that looks right:
 *
 * -query AND -fragment ARE TAKEN FROM THE URL'S OWN STRING, not from CFURLCopyQueryString/
 * CFURLCopyFragment: THOSE TWO UNESCAPE WHAT THEY RETURN, while this side of the bridge answers the
 * spelling as it stands -- the base class reads the component out of the URL string and leaves the
 * percent-escapes in it. Handing back a decoded query where a Foundation URL hands back an encoded one
 * would be a difference no caller could see coming, so the component is cut out between its delimiters.
 *
 * -fnCache ANSWERS NIL, AND THAT IS THE ONE PRIMITIVE WITH NO CORE FOUNDATION COUNTERPART: a CFURL has no
 * cache of measured resource values, and Core Foundation's own resource lookup goes to the file system.
 * The doors in libagora-base now go through this method rather than through the ivar, so nil reads as
 * "nothing was cached" rather than as uninitialised storage. */

/* THE +1-TO-+0 HELPER, SHARED BY EVERY ROW THAT NEEDS IT. A Copy-shaped Core Foundation door answers +1
 * and the methods in this table answer +0, so the owned object goes to -autorelease -- which for these
 * classes defers to the runtime's pool and drains into CFRelease (see the retain family above for why
 * that is now a release rather than the no-op it used to be). CFError's -userInfo needs the same step on
 * a dictionary, which is why the parameter is a CFTypeRef and not a CFStringRef. */
static id cf_bridge_autorelease(CFTypeRef owned)
{
	return owned == NULL ? nil : [(id)owned autorelease];
}

static id cf_imp_url_absolute_string(id self, SEL _cmd)
{
	(void)_cmd;
	/* GET, SO NOT OWNED: the URL's own string, which is what the base class's -absoluteString answers. */
	return (id)CFURLGetString((CFURLRef)self);
}

static id cf_imp_url_encoded_path(id self, SEL _cmd)
{
	(void)_cmd;
	/* CFURLCopyPath IS THE ENCODED SPELLING, which is the primitive side of this library's rule: -path
	 * decodes it. Nil for an opaque URL, and -path turns that into the empty string the base class
	 * documents. */
	return cf_bridge_autorelease(CFURLCopyPath((CFURLRef)self));
}

static id cf_imp_url_scheme(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease(CFURLCopyScheme((CFURLRef)self));
}

static id cf_imp_url_host(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease(CFURLCopyHostName((CFURLRef)self));
}

static id cf_imp_url_user(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease(CFURLCopyUserName((CFURLRef)self));
}

static id cf_imp_url_password(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease(CFURLCopyPassword((CFURLRef)self));
}

static id cf_imp_url_port(id self, SEL _cmd)
{
	SInt32 port = CFURLGetPortNumber((CFURLRef)self);

	(void)_cmd;
	/* MINUS ONE IS "NO PORT" (CFURL.h), and nil is this method's answer for it. The number is made with
	 * Core Foundation's own constructor: a CFNumber IS an NSNumber through this bridge, and CFNumber's
	 * owned class already answers -intValue. */
	return port < 0 ? nil : [(id)CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &port) autorelease];
}

/* THE QUERY AND THE FRAGMENT, CUT OUT OF THE URL'S STRING -- see the note above. The terminator is the
 * delimiter of the component that FOLLOWS (a fragment ends the query); NULL means "to the end". */
static id cf_imp_url_component(id self, CFStringRef separator, CFStringRef terminator)
{
	CFStringRef whole = CFURLGetString((CFURLRef)self);
	CFIndex length;
	CFRange found;
	CFRange tail;
	CFStringRef piece;

	if (whole == NULL) {
		return nil;
	}
	length = CFStringGetLength(whole);
	tail = CFRangeMake(0, length);
	if (!CFStringFindWithOptions(whole, separator, tail, 0, &found)) {
		return nil;
	}
	tail = CFRangeMake(found.location + found.length, length - (found.location + found.length));
	piece = NULL;
	if (terminator != NULL) {
		CFRange end;

		if (CFStringFindWithOptions(whole, terminator, tail, 0, &end)) {
			piece = CFStringCreateWithSubstring(kCFAllocatorDefault, whole,
							    CFRangeMake(tail.location, end.location - tail.location));
		}
	}
	if (piece == NULL) {
		piece = CFStringCreateWithSubstring(kCFAllocatorDefault, whole, tail);
	}
	return cf_bridge_autorelease(piece);
}

static id cf_imp_url_query(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_imp_url_component(self, CFSTR("?"), CFSTR("#"));
}

static id cf_imp_url_fragment(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_imp_url_component(self, CFSTR("#"), NULL);
}

static char cf_imp_url_is_file(id self, SEL _cmd)
{
	(void)_cmd;
	return (char)(_CFURLIsFileURL((CFURLRef)self) ? 1 : 0);
}

static id cf_imp_url_no_cache(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return nil;	/* a CFURL caches nothing: see the note above */
}

static const cf_own_method cf_url_methods[] = {
	{ "fnEncodedPath", (IMP)cf_imp_url_encoded_path, "@@:" },
	{ "scheme", (IMP)cf_imp_url_scheme, "@@:" },
	{ "host", (IMP)cf_imp_url_host, "@@:" },
	{ "user", (IMP)cf_imp_url_user, "@@:" },
	{ "password", (IMP)cf_imp_url_password, "@@:" },
	{ "port", (IMP)cf_imp_url_port, "@@:" },
	{ "query", (IMP)cf_imp_url_query, "@@:" },
	{ "fragment", (IMP)cf_imp_url_fragment, "@@:" },
	{ "absoluteString", (IMP)cf_imp_url_absolute_string, "@@:" },
	{ "relativeString", (IMP)cf_imp_url_absolute_string, "@@:" },
	{ "isFileURL", (IMP)cf_imp_url_is_file, "C@:" },
	{ "fnCache", (IMP)cf_imp_url_no_cache, "@@:" },
	{ NULL, NULL, NULL },
};

/* NSLocale PRIMITIVES, AND THERE IS EXACTLY ONE -- the shortest row in this table, and the one that shows
 * what the table is FOR. NSLocale is a single class with a single ivar (the canonical identifier), so
 * libagora-base needed ONE method routed through `self` (see NSLocale.m) and every other door on the
 * class -- the language, script, region and variant codes, the ICU-backed display names and currency and
 * number symbols, the separators, -isEqual:, -hash, -description -- rides on it. That is the same shape
 * CFDate and CFNull have at the other end of the scale: what a type costs is the size of its PRIMITIVE
 * set, and this one's is one. */
static id cf_imp_locale_identifier(id self, SEL _cmd)
{
	(void)_cmd;
	return (id)CFLocaleGetIdentifier((CFLocaleRef)self);
}

static const cf_own_method cf_locale_methods[] = {
	{ "localeIdentifier", (IMP)cf_imp_locale_identifier, "@@:" },
	{ NULL, NULL, NULL },
};

/* NSError PRIMITIVES, AND THERE ARE THREE -- the three ivars the class has. NSLocale was one and this is
 * three, and the same rule holds: -localizedDescription, -localizedFailureReason, the underlying-error
 * walks, -isEqual:, -hash, -description and the coder doors are all written over these, so a Core
 * Foundation-owned error inherits them. */
static id cf_imp_error_domain(id self, SEL _cmd)
{
	(void)_cmd;
	/* GET, SO NOT OWNED. */
	return (id)CFErrorGetDomain((CFErrorRef)self);
}

static long cf_imp_error_code(id self, SEL _cmd)
{
	(void)_cmd;
	return (long)CFErrorGetCode((CFErrorRef)self);
}

static id cf_imp_error_user_info(id self, SEL _cmd)
{
	(void)_cmd;
	/* COPY-SHAPED, and Core Foundation answers with an EMPTY dictionary rather than NULL when the error
	 * carries none -- which is a better answer than the base class gives for that case and the one the
	 * owned class will answer. */
	return cf_bridge_autorelease((CFTypeRef)CFErrorCopyUserInfo((CFErrorRef)self));
}

static const cf_own_method cf_error_methods[] = {
	{ "domain", (IMP)cf_imp_error_domain, "@@:" },
	{ "code", (IMP)cf_imp_error_code, "q@:" },
	{ "userInfo", (IMP)cf_imp_error_user_info, "@@:" },
	{ NULL, NULL, NULL },
};

/* NSAttributedString PRIMITIVES, AND THERE ARE TWO -- the string and the run table's read door. NSAttributedString
 * is not a class cluster: it is one class whose runs are its own storage, so libagora-base needed those two routed
 * through `self` and nothing else, because every remaining reader of the run table turned out to be a run-table
 * INTERNAL (`fnRunAt:`, `fnSplitAt:`, `fnReserve:`, ...) or an initialiser -- and the four public methods that
 * walked the table directly (`-initWithAttributedString:`, `-attributedSubstringFromRange:`,
 * -enumerateAttributesInRange:options:usingBlock:` and `-encodeWithCoder:`) now walk the PRIMITIVE instead.
 *
 * THE MUTABLE HALF IS NOT COVERED AND CANNOT BE FROM HERE, the same limitation CFArray's note describes: a
 * CFAttributedStringRef and a CFMutableAttributedStringRef share ONE type id, so an owned class cannot tell them
 * apart, and a CFMutableAttributedString would answer -replaceCharactersInRange:withString: with an inherited
 * NSMutableAttributedString implementation reading Core Foundation storage.
 *
 * NO PRIVATE DOOR IS NEEDED, WHICH IS WHAT MADE THIS TYPE THE CHEAPEST OF THE SIX LEFT: the one selector Core
 * Foundation asks for that base had not already written (_createAttributedSubstringWithRange:) was implemented in
 * a previous pass. */
static id cf_imp_attributed_string(id self, SEL _cmd)
{
	(void)_cmd;
	/* GET, SO NOT OWNED. */
	return (id)CFAttributedStringGetString((CFAttributedStringRef)self);
}

/* THE RANGE IS FILLED FIELD BY FIELD rather than by handing Core Foundation the caller's pointer: the caller may
 * pass NULL for it (both sides allow that), and one local out-parameter covers both cases in one call. */
static id cf_imp_attributed_attributes(id self, SEL _cmd, unsigned long location, NSRange *effectiveRange)
{
	CFRange range = CFRangeMake(0, 0);
	CFDictionaryRef attributes;

	(void)_cmd;
	attributes = CFAttributedStringGetAttributes((CFAttributedStringRef)self, (CFIndex)location, &range);
	if (effectiveRange != NULL) {
		effectiveRange->location = (unsigned long)range.location;
		effectiveRange->length = (unsigned long)range.length;
	}
	return (id)attributes;
}

/* AND -length, WHICH IS THE DOOR THIS BRIDGE'S OWN READS GO THROUGH AND WHICH WAS MISSING FROM BOTH ROWS.
 * MEASURED: Foundation's -mutableString proxy reads its owner with [_owner length] and [_owner string], both
 * PUBLIC doors -- so the fix belongs here and not in the proxy, whose writes already worked. Without this,
 * [_owner length] on a Core Foundation-backed attributed string ran FOUNDATION's implementation over
 * FOUNDATION's storage and answered 0, which made the proxy describe itself as EMPTY, made -appendString:
 * through it a no-op, and yet left -replaceCharactersInRange:withString: working. Reads wrong, writes partly. */
static unsigned long cf_imp_attributed_length(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFAttributedStringGetLength((CFAttributedStringRef)self);
}

static const cf_own_method cf_attributed_string_methods[] = {
	{ "string", (IMP)cf_imp_attributed_string, "@@:" },
	{ "length", (IMP)cf_imp_attributed_length, "Q@:" },
	{ "attributesAtIndex:effectiveRange:", (IMP)cf_imp_attributed_attributes, "@@:Q^{_NSRange=QQ}" },
	{ NULL, NULL, NULL },
};

/* ---------------------------------------------------------------------------------------------------- */
/* THE MUTABLE HALF OF AN ATTRIBUTED STRING: A STRING PLUS A RUN ARRAY, AND ONE DOOR THAT CANNOT BE      */
/* ---------------------------------------------------------------------------------------------------- */
/* MEASURED, IN THE ONE PLACE IT MATTERS: libagora-base's NSMutableAttributedString keeps a NSMutableString
 * and a run array, and its ONLY storage-writing method is -replaceCharactersInRange:withString: -- the run
 * helper -fnShiftRunsAfter:by: has exactly one caller, the same method (found by scanning for the helper's
 * CALLERS as well as the field names, which is the rule the character set's turn had to learn). Every other
 * mutation is written over it or over the attribute doors, and all of them are answered here rather than
 * left to Foundation, because Foundation's implementations write ITS storage into a Core Foundation object.
 *
 * A FOUNDATION ATTRIBUTED STRING ARRIVES AS AN ARGUMENT AND IS RECONSTRUCTED, NOT CAST. Unlike the
 * character sets there is no shared wire form to convert through, so the conversion walks the IMMUTABLE
 * primitive pair this class already answers -- -string and -attributesAtIndex:effectiveRange: -- and builds
 * a Core Foundation attributed string run by run. Casting one is what segfaulted the character sets.
 *
 * -mutableString IS NOT WHAT I FIRST RECORDED HERE, AND THE FIRST VERSION OF THIS COMMENT WAS WRONG ABOUT
 * CORE FOUNDATION. It said Core Foundation "keeps its own mutable string internally without offering any
 * way to hand it out". CORE FOUNDATION'S OWN HEADER DECLARES THAT ACCESSOR: CFAttributedStringGetMutableString,
 * which "gets the string ... as a mutable string, allowing editing the character contents ... Attributes
 * corresponding to the edited range are appropriately modified". MEASURED, and this is what is true:
 *
 *   1. CFAttributedStringGetMutableString DISPATCHES to -mutableString for a Foundation attributed string,
 *      and a Foundation one answers it with a view.
 *   2. For CORE FOUNDATION'S OWN attributed string the native path is COMMENTED OUT in CFAttributedString.c
 *      and the function returns NULL -- an upstream gap in this tree, not this bridge's.
 *   3. OUR OWNED CLASS therefore answers NULL through Core Foundation's door (it is Core Foundation's own
 *      object, so the native path runs) -- which matches what Core Foundation does for its own objects.
 *   4. BUT ITS -mutableString IS STILL REACHABLE THROUGH FOUNDATION, inherited from NSMutableAttributedString,
 *      and it is INCONSISTENT: MEASURED, the view it returns describes itself as EMPTY, -appendString: on it
 *      changes nothing, and -replaceCharactersInRange:withString: on it DOES change the attributed string.
 *      Reads wrong, writes partly -- which is worse than refusing, and is why the suite case drives neither
 *      and says so.
 *
 * SO THE HONEST STATUS IS NOT "cannot be bridged" -- IT IS FIXED, AND THE FIX WAS ON BOTH SIDES. Foundation's
 * proxy (FNAttributedStringBridge) reads its owner with [_owner length] and [_owner string], which are PUBLIC
 * doors, so -length had to be answered here -- it was missing from BOTH attributed rows, which is what made
 * the proxy read 0 and describe itself as empty. And the proxy had to answer the three doors its own
 * superclass actually uses (-UTF8String, -setString:, -appendUTF8String:) rather than only the Cocoa
 * primitives, because this library's mutable string keeps its storage behind those and a subclass that
 * answers only the Cocoa names gets ITS OWN storage used instead.
 *
 * MEASURED AFTER BOTH FIXES: the view describes "hello", -appendString: on it reaches the attributed string
 * (CF length 5 -> 6), -replaceCharactersInRange: still does, and the two agree afterwards. A Core Foundation
 * attributed string now hands out a working mutable-string view through Foundation's door.
 */

static CFAttributedStringRef cf_attributed_from_foundation(id other)
{
	CFMutableAttributedStringRef built;
	NSUInteger length, index;

	if (other == nil) {
		return NULL;
	}
	built = CFAttributedStringCreateMutable(NULL, 0);
	if (built == NULL) {
		return NULL;
	}
	CFAttributedStringReplaceString(built, CFRangeMake(0, 0), (CFStringRef)[other string]);
	length = (NSUInteger)[other length];
	index = 0;
	while (index < length) {
		NSRange effective = NSMakeRange(0, 0);
		NSDictionary *attrs = (NSDictionary *)[other attributesAtIndex:index effectiveRange:&effective];

		if (effective.length == 0) {
			break;
		}
		if (attrs != nil) {
			CFAttributedStringSetAttributes(built, CFRangeMake((CFIndex)index, (CFIndex)effective.length),
							(CFDictionaryRef)attrs, true);
		}
		index += effective.length;
	}
	return (CFAttributedStringRef)built;
}

static void cf_imp_mutable_attributed_set_string(id self, SEL _cmd, NSRange range, id str)
{
	(void)_cmd;
	CFAttributedStringReplaceString((CFMutableAttributedStringRef)self,
					CFRangeMake((CFIndex)range.location, (CFIndex)range.length), (CFStringRef)str);
}

static void cf_imp_mutable_attributed_replace_attributed(id self, SEL _cmd, NSRange range, id other)
{
	CFAttributedStringRef converted;

	(void)_cmd;
	converted = cf_attributed_from_foundation(other);
	if (converted != NULL) {
		CFAttributedStringReplaceAttributedString((CFMutableAttributedStringRef)self,
							  CFRangeMake((CFIndex)range.location, (CFIndex)range.length), converted);
		CFRelease(converted);
	}
}

static void cf_imp_mutable_attributed_insert(id self, SEL _cmd, id other, unsigned long loc)
{
	cf_imp_mutable_attributed_replace_attributed(self, _cmd, NSMakeRange((NSUInteger)loc, 0), other);
}

static void cf_imp_mutable_attributed_append(id self, SEL _cmd, id other)
{
	CFIndex length = CFAttributedStringGetLength((CFAttributedStringRef)self);

	cf_imp_mutable_attributed_replace_attributed(self, _cmd, NSMakeRange((NSUInteger)length, 0), other);
}

static void cf_imp_mutable_attributed_delete(id self, SEL _cmd, NSRange range)
{
	(void)_cmd;
	CFAttributedStringReplaceString((CFMutableAttributedStringRef)self,
					CFRangeMake((CFIndex)range.location, (CFIndex)range.length), CFSTR(""));
}

static void cf_imp_mutable_attributed_set_attributed(id self, SEL _cmd, id other)
{
	CFIndex length = CFAttributedStringGetLength((CFAttributedStringRef)self);

	cf_imp_mutable_attributed_replace_attributed(self, _cmd, NSMakeRange(0, (NSUInteger)length), other);
}

static void cf_imp_mutable_attributed_add_attribute(id self, SEL _cmd, id name, id value, NSRange range)
{
	(void)_cmd;
	CFAttributedStringSetAttribute((CFMutableAttributedStringRef)self,
				       CFRangeMake((CFIndex)range.location, (CFIndex)range.length),
				       (CFStringRef)name, (CFTypeRef)value);
}

static void cf_imp_mutable_attributed_add_attributes(id self, SEL _cmd, id attrs, NSRange range)
{
	(void)_cmd;
	CFAttributedStringSetAttributes((CFMutableAttributedStringRef)self,
					CFRangeMake((CFIndex)range.location, (CFIndex)range.length),
					(CFDictionaryRef)attrs, false);
}

static void cf_imp_mutable_attributed_set_attributes(id self, SEL _cmd, id attrs, NSRange range)
{
	(void)_cmd;
	CFAttributedStringSetAttributes((CFMutableAttributedStringRef)self,
					CFRangeMake((CFIndex)range.location, (CFIndex)range.length),
					(CFDictionaryRef)attrs, true);
}

static void cf_imp_mutable_attributed_remove_attribute(id self, SEL _cmd, id name, NSRange range)
{
	(void)_cmd;
	CFAttributedStringRemoveAttribute((CFMutableAttributedStringRef)self,
					  CFRangeMake((CFIndex)range.location, (CFIndex)range.length), (CFStringRef)name);
}

static void cf_imp_mutable_attributed_begin_editing(id self, SEL _cmd)
{
	(void)_cmd;
	CFAttributedStringBeginEditing((CFMutableAttributedStringRef)self);
}

static void cf_imp_mutable_attributed_end_editing(id self, SEL _cmd)
{
	(void)_cmd;
	CFAttributedStringEndEditing((CFMutableAttributedStringRef)self);
}

static id cf_imp_mutable_attributed_init(id self, SEL _cmd)
{
	(void)_cmd;
	return self;
}

static const cf_own_method cf_mutable_attributed_string_methods[] = {
	{ "string", (IMP)cf_imp_attributed_string, "@@:" },
	{ "length", (IMP)cf_imp_attributed_length, "Q@:" },
	{ "attributesAtIndex:effectiveRange:", (IMP)cf_imp_attributed_attributes, "@@:Q^{_NSRange=QQ}" },
	{ "replaceCharactersInRange:withString:", (IMP)cf_imp_mutable_attributed_set_string, "v@:{_NSRange=QQ}@" },
	{ "replaceCharactersInRange:withAttributedString:", (IMP)cf_imp_mutable_attributed_replace_attributed, "v@:{_NSRange=QQ}@" },
	{ "insertAttributedString:atIndex:", (IMP)cf_imp_mutable_attributed_insert, "v@:@@Q" },
	{ "appendAttributedString:", (IMP)cf_imp_mutable_attributed_append, "v@:@" },
	{ "deleteCharactersInRange:", (IMP)cf_imp_mutable_attributed_delete, "v@:{_NSRange=QQ}" },
	{ "setAttributedString:", (IMP)cf_imp_mutable_attributed_set_attributed, "v@:@" },
	{ "addAttribute:value:range:", (IMP)cf_imp_mutable_attributed_add_attribute, "v@:@@{_NSRange=QQ}" },
	{ "addAttributes:range:", (IMP)cf_imp_mutable_attributed_add_attributes, "v@:@{_NSRange=QQ}" },
	{ "setAttributes:range:", (IMP)cf_imp_mutable_attributed_set_attributes, "v@:@{_NSRange=QQ}" },
	{ "removeAttribute:range:", (IMP)cf_imp_mutable_attributed_remove_attribute, "v@:@{_NSRange=QQ}" },
	{ "beginEditing", (IMP)cf_imp_mutable_attributed_begin_editing, "v@:" },
	{ "endEditing", (IMP)cf_imp_mutable_attributed_end_editing, "v@:" },
	{ "init", (IMP)cf_imp_mutable_attributed_init, "@@:" },
	{ NULL, NULL, NULL },
};

static char cf_imp_charset_character_member(id self, SEL _cmd, unsigned short character)
{
	(void)_cmd;
	return (char)(CFCharacterSetIsCharacterMember((CFCharacterSetRef)self, (UniChar)character) ? 1 : 0);
}

static char cf_imp_charset_long_character_member(id self, SEL _cmd, unsigned int character)
{
	(void)_cmd;
	return (char)(CFCharacterSetIsLongCharacterMember((CFCharacterSetRef)self, (UTF32Char)character) ? 1 : 0);
}

static char cf_imp_charset_member_in_plane(id self, SEL _cmd, unsigned char plane)
{
	(void)_cmd;
	return (char)(CFCharacterSetHasMemberInPlane((CFCharacterSetRef)self, (uint8_t)plane) ? 1 : 0);
}

static id cf_imp_charset_bitmap(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease((CFTypeRef)CFCharacterSetCreateBitmapRepresentation(kCFAllocatorDefault,
											 (CFCharacterSetRef)self));
}

static id cf_imp_charset_inverted(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease((CFTypeRef)CFCharacterSetCreateInvertedSet(kCFAllocatorDefault,
										(CFCharacterSetRef)self));
}

static id cf_imp_charset_mutable_copy(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease((CFTypeRef)CFCharacterSetCreateMutableCopy(kCFAllocatorDefault,
									        (CFCharacterSetRef)self));
}

static CFCharacterSetRef cf_charset_from_foundation_set(id other);

/* THE COMPARISON DOOR, AND IT WAS MISSING FROM BOTH ROWS. libagora-base writes -isEqual: OVER
 * -isSupersetOfSet: in both directions (its own NSCharacterSet.h says so), so a Core Foundation-backed set
 * that does not answer -isSupersetOfSet: falls back to Foundation's implementation, reads FOUNDATION's
 * range list out of a Core Foundation object, and reports that two equal sets are unequal. MEASURED: that
 * is what it did, and it PREDATES the mutable half -- the immutable row never had this door, which is why
 * the immutable case in the suite passes without ever comparing two sets for equality. */
static unsigned char cf_imp_charset_is_superset(id self, SEL _cmd, id other)
{
	CFCharacterSetRef converted;
	unsigned char answer = 0;

	(void)_cmd;
	converted = cf_charset_from_foundation_set(other);
	if (converted != NULL) {
		answer = CFCharacterSetIsSupersetOfSet((CFCharacterSetRef)self, converted) ? 1 : 0;
		CFRelease(converted);
	}
	return answer;
}

static const cf_own_method cf_charset_methods[] = {
	{ "characterIsMember:", (IMP)cf_imp_charset_character_member, "C@:S" },
	{ "longCharacterIsMember:", (IMP)cf_imp_charset_long_character_member, "C@:I" },
	{ "hasMemberInPlane:", (IMP)cf_imp_charset_member_in_plane, "C@:C" },
	{ "bitmapRepresentation", (IMP)cf_imp_charset_bitmap, "@@:" },
	{ "invertedSet", (IMP)cf_imp_charset_inverted, "@@:" },
	{ "mutableCopy", (IMP)cf_imp_charset_mutable_copy, "@@:" },
	{ "isSupersetOfSet:", (IMP)cf_imp_charset_is_superset, "C@:@" },
	{ NULL, NULL, NULL },
};

/* ---------------------------------------------------------------------------------------------------- */
/* THE MUTABLE HALF OF A CHARACTER SET: THE FIVE METHODS THAT TOUCH FOUNDATION'S RANGES                  */
/* ---------------------------------------------------------------------------------------------------- */
/* FIVE, AND THEY WERE FOUND BY SCANNING WITH THE REAL FIELD NAMES. libagora-base keeps a character set as
 * a RANGE LIST (_ranges, _rangeCount, _capacity), and MEASURED -- with those names, uncapped -- exactly
 * five methods of NSMutableCharacterSet touch it: -formUnionWithCharacterSet:,
 * -formIntersectionWithCharacterSet:, -invert, -removeCharactersInRange: and -removeCharactersInString:.
 * The four add/remove-STRING/range doors Foundation also has are DERIVED (they build a set and call one of
 * these), so they need no entry. The six entries above are the immutable class's own, and a mutable set
 * must read as well as write.
 *
 * AN EARLIER SCAN OF MINE MISSED -removeCharactersInString: AND IT WAS MY PATTERN'S FAULT: I searched for
 * `_count`, which does not match `_rangeCount`. The list above is what a scan with the class's OWN field
 * names returns. */
/* THE OTHER SET ARRIVES AS A FOUNDATION OBJECT, SO IT IS CONVERTED THROUGH THE BITMAP RATHER THAN CAST.
 * MEASURED, AND IT COST A SEGFAULT: CFCharacterSetUnion reads its second argument as Core Foundation's own
 * struct, and a Foundation set is not one -- Core Foundation's C doors dispatch for the types whose doors
 * are bridged, and this one does not. The two sides already agree on the bitmap's format (Core
 * Foundation's bare 8192-byte plane-0 form, a decision this bridge made earlier), so a Core Foundation set
 * built from it is the same set, and it is released again immediately. */
static CFCharacterSetRef cf_charset_from_foundation_set(id other)
{
	NSData *bitmap = (other != nil) ? (NSData *)[other bitmapRepresentation] : nil;

	if (bitmap == nil) {
		return NULL;
	}
	return CFCharacterSetCreateWithBitmapRepresentation(NULL, (CFDataRef)bitmap);
}

static void cf_imp_mutable_charset_form_union(id self, SEL _cmd, id other)
{
	CFCharacterSetRef converted;

	(void)_cmd;
	converted = cf_charset_from_foundation_set(other);
	if (converted != NULL) {
		CFCharacterSetUnion((CFMutableCharacterSetRef)self, converted);
		CFRelease(converted);
	}
}

static void cf_imp_mutable_charset_form_intersection(id self, SEL _cmd, id other)
{
	CFCharacterSetRef converted;

	(void)_cmd;
	converted = cf_charset_from_foundation_set(other);
	if (converted != NULL) {
		CFCharacterSetIntersect((CFMutableCharacterSetRef)self, converted);
		CFRelease(converted);
	}
}

static void cf_imp_mutable_charset_invert(id self, SEL _cmd)
{
	(void)_cmd;
	CFCharacterSetInvert((CFMutableCharacterSetRef)self);
}

static void cf_imp_mutable_charset_remove_range(id self, SEL _cmd, NSRange range)
{
	(void)_cmd;
	CFCharacterSetRemoveCharactersInRange((CFMutableCharacterSetRef)self,
					      CFRangeMake((CFIndex)range.location, (CFIndex)range.length));
}

static void cf_imp_mutable_charset_add_string(id self, SEL _cmd, id string)
{
	(void)_cmd;
	CFCharacterSetAddCharactersInString((CFMutableCharacterSetRef)self, (CFStringRef)string);
}

static void cf_imp_mutable_charset_remove_string(id self, SEL _cmd, id string)
{
	(void)_cmd;
	CFCharacterSetRemoveCharactersInString((CFMutableCharacterSetRef)self, (CFStringRef)string);
}

static id cf_imp_mutable_charset_init(id self, SEL _cmd)
{
	(void)_cmd;
	return self;
}

static const cf_own_method cf_mutable_charset_methods[] = {
	{ "characterIsMember:", (IMP)cf_imp_charset_character_member, "C@:S" },
	{ "longCharacterIsMember:", (IMP)cf_imp_charset_long_character_member, "C@:I" },
	{ "hasMemberInPlane:", (IMP)cf_imp_charset_member_in_plane, "C@:C" },
	{ "bitmapRepresentation", (IMP)cf_imp_charset_bitmap, "@@:" },
	{ "invertedSet", (IMP)cf_imp_charset_inverted, "@@:" },
	{ "mutableCopy", (IMP)cf_imp_charset_mutable_copy, "@@:" },
	{ "isSupersetOfSet:", (IMP)cf_imp_charset_is_superset, "C@:@" },
	{ "formUnionWithCharacterSet:", (IMP)cf_imp_mutable_charset_form_union, "v@:@" },
	{ "formIntersectionWithCharacterSet:", (IMP)cf_imp_mutable_charset_form_intersection, "v@:@" },
	{ "invert", (IMP)cf_imp_mutable_charset_invert, "v@:" },
	{ "removeCharactersInRange:", (IMP)cf_imp_mutable_charset_remove_range, "v@:{_NSRange=QQ}" },
	{ "addCharactersInString:", (IMP)cf_imp_mutable_charset_add_string, "v@:@" },
	{ "removeCharactersInString:", (IMP)cf_imp_mutable_charset_remove_string, "v@:@" },
	{ "init", (IMP)cf_imp_mutable_charset_init, "@@:" },
	{ NULL, NULL, NULL },
};

/* THE TWO STREAM TYPES, AND THIS PAIR IS THE SHORTEST JOB IN THE WHOLE TABLE -- BECAUSE THE CONTRACT ALREADY
 * COVERS THE STORAGE. Every method on NSInputStream/NSOutputStream that reads the stream's own ivars
 * (-read:maxLength:, -getBuffer:length:, -hasBytesAvailable, -propertyForKey:, -setProperty:forKey:, -open,
 * -close) is ALSO the method Core Foundation asks for by name, so nothing in libagora-base had to be routed
 * through `self` for these two: the table below IS the routing.
 *
 * THE TWO EXCEPTIONS ARE FOUNDATION'S OWN, and they answer what is TRUE of a Core Foundation stream seen
 * through them. -fnStreamDescriptor and -fnStreamEventForReadiness are what Foundation's run-loop integration
 * asks a stream for; a CF stream has neither, because Core Foundation's own run loop is what watches it. -1
 * and NSStreamEventNone are the answers an unopened Foundation stream gives as well -- so a CF stream simply
 * never looks ready to Foundation's loop, which is the honest answer rather than a wrong file descriptor.
 *
 * -_cfStreamError AND -streamError ARE NOT HERE, and that is a gap on the OTHER side of the bridge rather than
 * this one: libagora-base implements neither, so Core Foundation's dispatch to them raises for a FOUNDATION
 * stream today. Nothing in this direction needs them. */
static unsigned long cf_imp_read_stream_status(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFReadStreamGetStatus((CFReadStreamRef)self);
}

static long cf_imp_read_stream_read(id self, SEL _cmd, char *buffer, unsigned long length)
{
	(void)_cmd;
	return (long)CFReadStreamRead((CFReadStreamRef)self, (UInt8 *)buffer, (CFIndex)length);
}

static char cf_imp_read_stream_has_bytes(id self, SEL _cmd)
{
	(void)_cmd;
	return (char)(CFReadStreamHasBytesAvailable((CFReadStreamRef)self) ? 1 : 0);
}

/* CFReadStreamGetBuffer ANSWERS THE POINTER AND WRITES THE COUNT, while this method answers a BOOLEAN and
 * writes both -- the same question in two shapes, so the answer is the pointer's non-nullness. The caller's
 * incoming `*length` is the most it wants to read, which is exactly Core Foundation's maxBytesToRead. */
static char cf_imp_read_stream_get_buffer(id self, SEL _cmd, const unsigned char **buffer, unsigned long *length)
{
	CFIndex used = 0;
	const UInt8 *bytes;

	(void)_cmd;
	bytes = CFReadStreamGetBuffer((CFReadStreamRef)self, (CFIndex)(length != NULL ? *length : 0), &used);
	if (bytes == NULL) {
		return 0;
	}
	if (buffer != NULL) {
		*buffer = bytes;
	}
	if (length != NULL) {
		*length = (unsigned long)used;
	}
	return 1;
}

static id cf_imp_read_stream_property(id self, SEL _cmd, id key)
{
	(void)_cmd;
	return cf_bridge_autorelease(CFReadStreamCopyProperty((CFReadStreamRef)self, (CFStringRef)key));
}

static char cf_imp_read_stream_set_property(id self, SEL _cmd, id property, id key)
{
	(void)_cmd;
	return (char)(CFReadStreamSetProperty((CFReadStreamRef)self, (CFStringRef)key, (CFTypeRef)property) ? 1 : 0);
}

static void cf_imp_read_stream_open(id self, SEL _cmd)
{
	(void)_cmd;
	(void)CFReadStreamOpen((CFReadStreamRef)self);
}

static void cf_imp_read_stream_close(id self, SEL _cmd)
{
	(void)_cmd;
	CFReadStreamClose((CFReadStreamRef)self);
}

static int cf_imp_stream_no_descriptor(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return -1;	/* see the note above: a CF stream has no descriptor for Foundation's loop */
}

static unsigned long cf_imp_read_stream_event(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 0;	/* NSStreamEventNone */
}

static const cf_own_method cf_read_stream_methods[] = {
	{ "streamStatus", (IMP)cf_imp_read_stream_status, "Q@:" },
	{ "read:maxLength:", (IMP)cf_imp_read_stream_read, "q@:*Q" },
	{ "hasBytesAvailable", (IMP)cf_imp_read_stream_has_bytes, "C@:" },
	{ "getBuffer:length:", (IMP)cf_imp_read_stream_get_buffer, "C@:^*^Q" },
	{ "propertyForKey:", (IMP)cf_imp_read_stream_property, "@@:@" },
	{ "setProperty:forKey:", (IMP)cf_imp_read_stream_set_property, "C@:@@" },
	{ "open", (IMP)cf_imp_read_stream_open, "v@:" },
	{ "close", (IMP)cf_imp_read_stream_close, "v@:" },
	{ "fnStreamDescriptor", (IMP)cf_imp_stream_no_descriptor, "i@:" },
	{ "fnStreamEventForReadiness", (IMP)cf_imp_read_stream_event, "Q@:" },
	{ NULL, NULL, NULL },
};

static unsigned long cf_imp_write_stream_status(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFWriteStreamGetStatus((CFWriteStreamRef)self);
}

static long cf_imp_write_stream_write(id self, SEL _cmd, const char *buffer, unsigned long length)
{
	(void)_cmd;
	return (long)CFWriteStreamWrite((CFWriteStreamRef)self, (const UInt8 *)buffer, (CFIndex)length);
}

static char cf_imp_write_stream_has_space(id self, SEL _cmd)
{
	(void)_cmd;
	/* CORE FOUNDATION CALLS THIS CanAcceptBytes, and it is the same question: whether a write would block. */
	return (char)(CFWriteStreamCanAcceptBytes((CFWriteStreamRef)self) ? 1 : 0);
}

static id cf_imp_write_stream_property(id self, SEL _cmd, id key)
{
	(void)_cmd;
	return cf_bridge_autorelease(CFWriteStreamCopyProperty((CFWriteStreamRef)self, (CFStringRef)key));
}

static char cf_imp_write_stream_set_property(id self, SEL _cmd, id property, id key)
{
	(void)_cmd;
	return (char)(CFWriteStreamSetProperty((CFWriteStreamRef)self, (CFStringRef)key, (CFTypeRef)property) ? 1 : 0);
}

static void cf_imp_write_stream_open(id self, SEL _cmd)
{
	(void)_cmd;
	(void)CFWriteStreamOpen((CFWriteStreamRef)self);
}

static void cf_imp_write_stream_close(id self, SEL _cmd)
{
	(void)_cmd;
	CFWriteStreamClose((CFWriteStreamRef)self);
}

static const cf_own_method cf_write_stream_methods[] = {
	{ "streamStatus", (IMP)cf_imp_write_stream_status, "Q@:" },
	{ "write:maxLength:", (IMP)cf_imp_write_stream_write, "q@:r*Q" },
	{ "hasSpaceAvailable", (IMP)cf_imp_write_stream_has_space, "C@:" },
	{ "propertyForKey:", (IMP)cf_imp_write_stream_property, "@@:@" },
	{ "setProperty:forKey:", (IMP)cf_imp_write_stream_set_property, "C@:@@" },
	{ "open", (IMP)cf_imp_write_stream_open, "v@:" },
	{ "close", (IMP)cf_imp_write_stream_close, "v@:" },
	{ "fnStreamDescriptor", (IMP)cf_imp_stream_no_descriptor, "i@:" },
	{ NULL, NULL, NULL },
};

/* NSTimer PRIMITIVES, AND THERE ARE TEN -- six that Core Foundation asks for by name (timeInterval, isValid,
 * invalidate, tolerance, setTolerance:), three that libagora-base's own doors are written over (fireDate and
 * setFireDate: through CFRunLoopTimerGet/SetNextFireDate, and fnRepeats, the private accessor -description
 * reads) and ONE that has no Core Foundation counterpart at all: -userInfo. A CF timer carries a CONTEXT --
 * a C callback and a pointer -- and not an Objective-C user-info object, so nil is the true answer and not a
 * stub.
 *
 * -fire IS ANSWERED WITH NOTHING, AND THAT IS WORTH SAYING PLAINLY: Core Foundation has no synchronous "fire
 * this now" door -- a CF timer's callout belongs to Core Foundation's own run loop -- so a caller who asks
 * this is told, by the absence of any effect, that there was nothing to do. It is the one door here whose
 * answer is a documented limit rather than a fact about the object.
 *
 * NSTimer's OTHER STATE IS NOT HERE BECAUSE IT IS NOT NSTIMER'S: the target/selector pair, the block and the
 * invocation live on the run loop's own private timer class (see NSRunLoop.m), whose -hasTarget:,
 * -targetIsAlive and -fireIfAlive are that class's protocol and not this one's -- measured, as
 * class_getInstanceMethod([NSTimer class], ...) answering nothing for all three. */
static double cf_imp_timer_interval(id self, SEL _cmd)
{
	(void)_cmd;
	return (double)CFRunLoopTimerGetInterval((CFRunLoopTimerRef)self);
}

static id cf_imp_timer_fire_date(id self, SEL _cmd)
{
	(void)_cmd;
	/* THE NEXT FIRE DATE IS CORE FOUNDATION'S ANSWER TO THE SAME QUESTION, and the object it comes back in is
	 * an NSDate -- a CFDate IS one through this bridge. */
	return [(id)CFDateCreate(kCFAllocatorDefault, CFRunLoopTimerGetNextFireDate((CFRunLoopTimerRef)self))
		 autorelease];
}

static void cf_imp_timer_set_fire_date(id self, SEL _cmd, id date)
{
	(void)_cmd;
	if (date != nil) {
		/* CFDateGetAbsoluteTime IS THE CONVERSION AND IT WORKS ON EITHER SIDE: for a Foundation NSDate it
		 * dispatches to -timeIntervalSinceReferenceDate, which is the primitive the CFDate owned class
		 * answers. */
		CFRunLoopTimerSetNextFireDate((CFRunLoopTimerRef)self, CFDateGetAbsoluteTime((CFDateRef)date));
	}
}

static char cf_imp_timer_is_valid(id self, SEL _cmd)
{
	(void)_cmd;
	return (char)(CFRunLoopTimerIsValid((CFRunLoopTimerRef)self) ? 1 : 0);
}

static void cf_imp_timer_invalidate(id self, SEL _cmd)
{
	(void)_cmd;
	CFRunLoopTimerInvalidate((CFRunLoopTimerRef)self);
}

static double cf_imp_timer_tolerance(id self, SEL _cmd)
{
	(void)_cmd;
	return (double)CFRunLoopTimerGetTolerance((CFRunLoopTimerRef)self);
}

static void cf_imp_timer_set_tolerance(id self, SEL _cmd, double tolerance)
{
	(void)_cmd;
	CFRunLoopTimerSetTolerance((CFRunLoopTimerRef)self, (CFTimeInterval)tolerance);
}

static char cf_imp_timer_repeats(id self, SEL _cmd)
{
	(void)_cmd;
	return (char)(CFRunLoopTimerDoesRepeat((CFRunLoopTimerRef)self) ? 1 : 0);
}

static id cf_imp_timer_user_info(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return nil;	/* a CF timer holds a context, not a user-info object: see the note above */
}

static void cf_imp_timer_fire(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;

	/* NOTHING, DELIBERATELY: see the note above. */
}

static const cf_own_method cf_timer_methods[] = {
	{ "timeInterval", (IMP)cf_imp_timer_interval, "d@:" },
	{ "fireDate", (IMP)cf_imp_timer_fire_date, "@@:" },
	{ "setFireDate:", (IMP)cf_imp_timer_set_fire_date, "v@:@" },
	{ "isValid", (IMP)cf_imp_timer_is_valid, "C@:" },
	{ "invalidate", (IMP)cf_imp_timer_invalidate, "v@:" },
	{ "tolerance", (IMP)cf_imp_timer_tolerance, "d@:" },
	{ "setTolerance:", (IMP)cf_imp_timer_set_tolerance, "v@:d" },
	{ "fnRepeats", (IMP)cf_imp_timer_repeats, "C@:" },
	{ "userInfo", (IMP)cf_imp_timer_user_info, "@@:" },
	{ "fire", (IMP)cf_imp_timer_fire, "v@:" },
	{ NULL, NULL, NULL },
};

/* NSCalendar PRIMITIVES, AND THERE ARE NINE -- FIVE READS AND FOUR WRITES, WHICH IS THE WHOLE STATE THE CLASS
 * HAS. NSCalendar is not a class cluster and keeps no computation cache: its ivars are the identifier, the
 * time zone, the locale and two week-boundary numbers, and EVERYTHING ELSE -- the component conversion, the
 * range and ordinality arithmetic, the symbol tables, the weekend logic, all of it ICU-backed -- is written
 * over the five accessors. That routing is libagora-base's half of this type, and it is what lets a Core
 * Foundation-owned calendar answer the computation doors at all.
 *
 * AND THIS ROW IS NOT VERIFIED, WHICH IS A FACT ABOUT CORE FOUNDATION RATHER THAN ABOUT THIS TABLE: a Core
 * Foundation calendar cannot be CONSTRUCTED in this environment at all. CFCalendarCreateWithIdentifier reaches
 * _CFCalendarInitialize, which asks CFTimeZoneCopyDefault for the current zone, and CFTimeZoneCopySystem
 * SEGFAULTS inside libc -- measured, with a backtrace carrying no Objective-C frame, and IT SURVIVES WITH THIS
 * ROW REMOVED, so it is a pre-existing defect of Core Foundation's system-zone lookup and not anything here.
 * (libagora-base handles the same environment: its +systemTimeZone answers GMT for ICU's "Etc/Unknown", which
 * is what a system with no /etc/localtime and no TZ means.) Every entry below is measured and compiled in, but
 * no case in the suite can reach them until that lookup is fixed -- the reverse-direction case that belongs
 * here is owed with it.
 *
 * THE PRIVATE ...atp DOORS ARE ALSO NOT HERE, AND THAT GAP IS ON THE OTHER SIDE OF THE BRIDGE: Core
 * Foundation's composition and decomposition doors dispatch to _composeAbsoluteTime:atp,
 * _decomposeAbsoluteTime:at, _addComponents:atp and their neighbours, which libagora-base does not implement
 * -- so those FORWARD doors raise for a Foundation calendar. Nothing in this direction needs them. */
static id cf_imp_calendar_identifier(id self, SEL _cmd)
{
	(void)_cmd;
	return (id)CFCalendarGetIdentifier((CFCalendarRef)self);
}

static id cf_imp_calendar_time_zone(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease((CFTypeRef)CFCalendarCopyTimeZone((CFCalendarRef)self));
}

static id cf_imp_calendar_locale(id self, SEL _cmd)
{
	(void)_cmd;
	return cf_bridge_autorelease((CFTypeRef)CFCalendarCopyLocale((CFCalendarRef)self));
}

static unsigned long cf_imp_calendar_first_weekday(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFCalendarGetFirstWeekday((CFCalendarRef)self);
}

static unsigned long cf_imp_calendar_minimum_days(id self, SEL _cmd)
{
	(void)_cmd;
	return (unsigned long)CFCalendarGetMinimumDaysInFirstWeek((CFCalendarRef)self);
}

static void cf_imp_calendar_set_time_zone(id self, SEL _cmd, id zone)
{
	(void)_cmd;
	CFCalendarSetTimeZone((CFCalendarRef)self, (CFTimeZoneRef)zone);
}

static void cf_imp_calendar_set_locale(id self, SEL _cmd, id locale)
{
	(void)_cmd;
	CFCalendarSetLocale((CFCalendarRef)self, (CFLocaleRef)locale);
}

static void cf_imp_calendar_set_first_weekday(id self, SEL _cmd, unsigned long weekday)
{
	(void)_cmd;
	CFCalendarSetFirstWeekday((CFCalendarRef)self, (CFIndex)weekday);
}

static void cf_imp_calendar_set_minimum_days(id self, SEL _cmd, unsigned long days)
{
	(void)_cmd;
	CFCalendarSetMinimumDaysInFirstWeek((CFCalendarRef)self, (CFIndex)days);
}

static const cf_own_method cf_calendar_methods[] = {
	{ "calendarIdentifier", (IMP)cf_imp_calendar_identifier, "@@:" },
	{ "timeZone", (IMP)cf_imp_calendar_time_zone, "@@:" },
	{ "locale", (IMP)cf_imp_calendar_locale, "@@:" },
	{ "firstWeekday", (IMP)cf_imp_calendar_first_weekday, "Q@:" },
	{ "minimumDaysInFirstWeek", (IMP)cf_imp_calendar_minimum_days, "Q@:" },
	{ "setTimeZone:", (IMP)cf_imp_calendar_set_time_zone, "v@:@" },
	{ "setLocale:", (IMP)cf_imp_calendar_set_locale, "v@:@" },
	{ "setFirstWeekday:", (IMP)cf_imp_calendar_set_first_weekday, "v@:Q" },
	{ "setMinimumDaysInFirstWeek:", (IMP)cf_imp_calendar_set_minimum_days, "v@:Q" },
	{ NULL, NULL, NULL },
};

/* THE MUTABLE SIBLINGS CANNOT BE COVERED FROM THIS TABLE, AND THEIR FAILURE IS LOUD RATHER THAN QUIET --
 * WHICH IS A MEASUREMENT AND NOT A HOPE. CFMutableArrayRef and CFArrayRef SHARE ONE TYPE ID (the same is
 * true of the mutable set, dictionary, string, data and attributed string), so the class built here cannot
 * tell them apart, and a row answers for both. What a caller gets today, measured on the objects this
 * table builds:
 *
 *     [(NSMutableArray *)CFArrayCreateMutable(...) addObject:@"c"]
 *         -> NSInvalidArgumentException: -[__NSCFArray addObject:] is not implemented
 *     [(NSMutableCharacterSet *)CFCharacterSetCreateMutable(NULL) addCharactersInString:@"z"]
 *         -> NSInvalidArgumentException: -[__NSCFCharacterSet addCharactersInString:] is not implemented
 *
 * AND CORE FOUNDATION'S OWN DOORS ON THE SAME OBJECT KEEP WORKING (measured: characterIsMember: answers
 * 'q' after CFCharacterSetAddCharactersInString), so nothing is corrupted -- the mutation simply is not
 * answered by Foundation. The other mutable types are the same shape and are NOT measured one by one.
 *
 * AN EARLIER COMMIT DESCRIBED A WORSE STATE AND THIS IS WHY THE DESCRIPTION CHANGED: while Core
 * Foundation's objects still wore FOUNDATION's class, a CFMutableArrayRef would have answered -addObject:
 * with an inherited NSMutableArray method reading Core Foundation storage -- a silent corruption. Giving
 * every type a class of Core Foundation's own is what turned that into a raise.
 *
 * APPLE HAS __NSCFArray AND __NSCFMutableArray AND PICKS AT CREATION, which is what the category parameter
 * of _CFRuntimeCreateInstance exists for (measured: the parameter is there, it is used only for OASafe
 * allocation naming, and CFArray.c passes NULL at both creation sites). AND THE FOUNDATION SIDE COMES
 * FIRST, WHICH IS NOT WHERE THIS LOOKS LIKE IT LEADS. A mutable owned class cannot be given Core
 * Foundation-reading primitives until Foundation's mutable class READS THROUGH PRIMITIVES, and the two have
 * to land together -- measured, because landing the class alone would be worse than the raise it replaces:
 *
 *   - AGArrayMutable (libagora-base/NSArray.m) implements NOTHING. Its own comment says why: "NSMutableArray's
 *     own implementation IS the mutable storage implementation, this class inherits it". So a subclass of
 *     NSMutableArray inherits code that reads _items/_count -- Foundation's storage -- and that code over a
 *     Core Foundation object is the SILENT CORRUPTION the CFArray commit warned about, which is exactly what
 *     giving every type its own class had turned into a loud raise.
 *   - NSMutableArray's implementation is 434 lines and 32 methods with 23 direct _items touches. Those are
 *     the sites that have to become calls to a small primitive set, the way NSArray's were routed to -count
 *     and -objectAtIndex: (the two primitives cf_array_methods implements below).
 *
 * SO THE ORDER IS: route Foundation's mutable array through primitives, then add the row and the category in
 * Core Foundation, then the same for the other six mutable types. Until the first step is done the mutable
 * half of these types is not bridged, and the loud raise is the honest ceiling. */
static const struct {
	CFTypeID type;
	const char *name;
	const cf_own_method *methods;
} cf_own_classes[] = {
	{ _kCFRuntimeIDCFString,     "__NSCFString",     cf_string_methods },
	{ _kCFRuntimeIDCFDate,       "__NSCFDate",       cf_date_methods },
	{ _kCFRuntimeIDCFNumber,     "__NSCFNumber",     cf_number_methods },
	{ _kCFRuntimeIDCFBoolean,    "__NSCFBoolean",    cf_boolean_methods },
	{ _kCFRuntimeIDCFArray,      "__NSCFArray",      cf_array_methods },
	{ _kCFRuntimeIDCFData,       "__NSCFData",       cf_data_methods },
	{ _kCFRuntimeIDCFDictionary, "__NSCFDictionary", cf_dictionary_methods },
	{ _kCFRuntimeIDCFSet,        "__NSCFSet",        cf_set_methods },
	{ _kCFRuntimeIDCFNull,       "__NSCFNull",       cf_null_methods },
	{ _kCFRuntimeIDCFTimeZone,   "__NSCFTimeZone",   cf_timezone_methods },
	{ _kCFRuntimeIDCFURL,        "__NSCFURL",        cf_url_methods },
	{ _kCFRuntimeIDCFLocale,     "__NSCFLocale",     cf_locale_methods },
	{ _kCFRuntimeIDCFError,      "__NSCFError",      cf_error_methods },
	{ _kCFRuntimeIDCFAttributedString, "__NSCFAttributedString", cf_attributed_string_methods },
	{ _kCFRuntimeIDCFCharacterSet, "__NSCFCharacterSet", cf_charset_methods },
	{ _kCFRuntimeIDCFReadStream, "__NSCFReadStream", cf_read_stream_methods },
	{ _kCFRuntimeIDCFWriteStream, "__NSCFWriteStream", cf_write_stream_methods },
	{ _kCFRuntimeIDCFRunLoopTimer, "__NSCFRunLoopTimer", cf_timer_methods },
	{ _kCFRuntimeIDCFCalendar, "__NSCFCalendar", cf_calendar_methods },
	{ 0, NULL, NULL },
};
#endif /* __OBJC__ */

#if defined(__OBJC__)
extern SEL sel_registerName(const char *name);

/* ---------------------------------------------------------------------------------------------------- */
static const struct {
	CFTypeID type;
	const char *name;
	const char *superclass;
	const cf_own_method *methods;
} cf_own_mutable_classes[] = {
	{ _kCFRuntimeIDCFArray, "__NSCFMutableArray", "NSMutableArray", cf_mutable_array_methods },
	{ _kCFRuntimeIDCFDictionary, "__NSCFMutableDictionary", "NSMutableDictionary", cf_mutable_dictionary_methods },
	{ _kCFRuntimeIDCFSet, "__NSCFMutableSet", "NSMutableSet", cf_mutable_set_methods },
	{ _kCFRuntimeIDCFString, "__NSCFMutableString", "NSMutableString", cf_mutable_string_methods },
	{ _kCFRuntimeIDCFData, "__NSCFMutableData", "NSMutableData", cf_mutable_data_methods },
	{ _kCFRuntimeIDCFCharacterSet, "__NSCFMutableCharacterSet", "NSMutableCharacterSet", cf_mutable_charset_methods },
	{ _kCFRuntimeIDCFAttributedString, "__NSCFMutableAttributedString", "NSMutableAttributedString", cf_mutable_attributed_string_methods },
	{ 0, NULL, NULL, NULL },
};

/* THE METHOD TYPES A ROW GIVES MUST BE THE FULL ENCODING FOR ANY SELECTOR WITH TWO ARGUMENTS OR MORE    */
/* ---------------------------------------------------------------------------------------------------- */
/* MEASURED, and it cost an afternoon: an owned class built by HAND with the ABBREVIATED encoding --
 * "v@:@@Q" for -insertObject:atIndex: -- receives NO SEND for that selector at all. The class is built,
 * class_getInstanceMethod resolves to the row's IMP, calling that IMP directly works, and the message
 * simply goes to Foundation's implementation instead. With the encoding method_getTypeEncoding returns for
 * FOUNDATION'S OWN method of the same selector -- "v32@0:8@16Q24" -- the send arrives as it should.
 *
 * THE DIFFERENCE IS THE FRAME: the abbreviated form has no offsets, and libobjc2 needs them to marshal two
 * arguments. Zero- and one-argument selectors ("Q@:" for -count, "v@:@" for -addObject:) behave with either
 * spelling, which is why every immutable row in the table above has always worked and why this stayed
 * hidden until a MUTABLE row needed a two-argument mutator.
 *
 * SO A ROW THAT HAS A FOUNDATION PROTOTYPE SHOULD TAKE ITS TYPES FROM THAT PROTOTYPE rather than from a
 * hand-written string: class_getInstanceMethod(superclass, sel) and method_getTypeEncoding of the result.
 * The immutable rows above keep their abbreviations because they are measured to work; a mutable row
 * answers Foundation methods, so it has a prototype for every selector it needs -- and -fnMutationWord,
 * which is Foundation's own private primitive, has one too. */
static uintptr_t cf_bridged_subclass(CFTypeID typeID, const void *cls_ref)
{
	const cf_own_method *methods = NULL;
	const char *name = NULL;
	Class sub;

	for (size_t i = 0; cf_own_classes[i].name != NULL; i++) {
		if (cf_own_classes[i].type == typeID) {
			name = cf_own_classes[i].name;
			methods = cf_own_classes[i].methods;
			break;
		}
	}
	if (name == NULL) {
		return 0;	/* not a type that has an owned class yet */
	}
	sub = objc_allocateClassPair((Class)cls_ref, name, 0);
	if (sub == NULL) {
		return 0;	/* the name is taken by a previous registration: keep what we have */
	}
	for (size_t i = 0; methods != NULL && methods[i].name != NULL; i++) {
		class_addMethod(sub, sel_registerName(methods[i].name), methods[i].imp, methods[i].types);
	}
	/* AND THE RETAIN FAMILY, WHICH IS THE SAME THREE METHODS ON EVERY ONE OF THEM. */
	for (size_t i = 0; cf_object_methods[i].name != NULL; i++) {
		class_addMethod(sub, sel_registerName(cf_object_methods[i].name), cf_object_methods[i].imp,
				cf_object_methods[i].types);
	}
	objc_registerClassPair(sub);
	/* RECORDED AS CORE FOUNDATION'S OWN, which is what CFRetain and CFRelease ask before deciding which
	 * reference count to touch. */
	if (__CFRuntimeOwnedClassCount < __CFRuntimeMaxOwnedClasses) {
		__CFRuntimeOwnedClasses[__CFRuntimeOwnedClassCount++] = (uintptr_t)sub;
	}
	/* AND A STATIC INSTANCE OF THIS TYPE GETS THE CLASS WRITTEN INTO IT, because it cannot have named one
	 * before now: it was initialised at COMPILE time and its isa is a compile-time NULL. kCFNull is the
	 * the STATIC instances of a bridged type get their class written in below: kCFNull, and CFBoolean's two
 * constants -- see __CFNullBindOwnedClass in CFBase.c and __CFBooleanBindOwnedClass in CFNumber.c. */
	if (typeID == _kCFRuntimeIDCFNull) {
		__CFNullBindOwnedClass((void *)sub);
	}
	/* AND CFBoolean HAS TWO OF THEM, which are its only instances: kCFBooleanTrue and kCFBooleanFalse. */
	if (typeID == _kCFRuntimeIDCFBoolean) {
		__CFBooleanBindOwnedClass((void *)sub);
	}
	/* AND THE MUTABLE HALF, IF THIS TYPE HAS ONE: the same construction over Foundation's MUTABLE class,
	 * with each method's TYPES TAKEN FROM FOUNDATION'S PROTOTYPE. */
	for (size_t m = 0; cf_own_mutable_classes[m].name != NULL; m++) {
		Class superclass;
		Class msub;

		if (cf_own_mutable_classes[m].type != typeID) {
			continue;
		}
		superclass = objc_getClass(cf_own_mutable_classes[m].superclass);
		if (superclass == NULL) {
			break;
		}
		msub = objc_allocateClassPair(superclass, cf_own_mutable_classes[m].name, 0);
		if (msub == NULL) {
			break;
		}
		for (size_t i = 0; cf_own_mutable_classes[m].methods[i].name != NULL; i++) {
			SEL sel = sel_registerName(cf_own_mutable_classes[m].methods[i].name);
			Method prototype = class_getInstanceMethod(superclass, sel);
			const char *types = (prototype != NULL) ? method_getTypeEncoding(prototype)
							       : cf_own_mutable_classes[m].methods[i].types;

			class_addMethod(msub, sel, cf_own_mutable_classes[m].methods[i].imp, types);
		}
		for (size_t i = 0; cf_object_methods[i].name != NULL; i++) {
			class_addMethod(msub, sel_registerName(cf_object_methods[i].name), cf_object_methods[i].imp,
					cf_object_methods[i].types);
		}
		objc_registerClassPair(msub);
		if (__CFRuntimeOwnedClassCount < __CFRuntimeMaxOwnedClasses) {
			__CFRuntimeOwnedClasses[__CFRuntimeOwnedClassCount++] = (uintptr_t)msub;
		}
		__CFRuntimeCFOwnMutableClassTable[typeID] = (uintptr_t)msub;
		break;
	}
	return (uintptr_t)sub;
}

/* AGORA: THE CLASS AN UNBRIDGED CORE FOUNDATION OBJECT WEARS, WHICH IS APPLE'S __NSCFType.
 *
 * cf_bridged_subclass gives a type a class only when Foundation hands one over. A type Foundation never
 * bridges -- every CoreText font, every CoreGraphics color space, CFUUID, CFBitVector, CFTree -- therefore
 * had isa == 0, and isa == 0 is not an object: libobjc2's objc_retain reads obj->isa before it decides
 * whether to send -retain, so Foundation's -setObject:forKey: dereferenced NULL. MEASURED, and it is what
 * this exists for: SIGSEGV in objc_test_class_flag <- objc_retain <- [NSDictionary setObject:forKey:],
 * with a CTFontRef in hand.
 *
 * APPLE GIVES EVERY CF TYPE A CLASS, __NSCFType when the type has none of its own, and Core Foundation
 * makes it here rather than Foundation because an unbridged object can be created before Foundation is
 * loaded. It is a SUBCLASS OF NSObject when the runtime already has one, so it inherits -isEqual:, -hash,
 * -class and the rest of the protocol Foundation sends an argument; WITHOUT NSObject IT IS NOT MADE AT ALL
 * and the object keeps isa == 0 -- where nothing can reach it, because nothing messages a Core Foundation
 * object until Foundation exists. NSObject's presence, not Foundation's, is the gate.
 *
 * ITS METHODS ARE cf_object_methods, THE SAME RETAIN FAMILY EVERY OWNED CLASS GETS. That is what makes
 * objc_retain dispatch -retain into CFRetain instead of walking the word before the object -- libobjc2's
 * own retain count, which over a CFRuntime allocation is the MALLOC CHUNK HEADER. And it is recorded in
 * __CFRuntimeOwnedClasses for the same reason: CFRetain and CFRelease must take the CORE FOUNDATION path
 * for anything wearing it.
 *
 * MADE ONCE, UNDER THE LOCK THAT GUARDS THE CLASS TABLES, AND ONLY AFTER NSObject EXISTS. The cheap
 * objc_getClass("NSObject") before the lock means a call that arrives too early returns 0 WITHOUT caching
 * a failure, so a later call -- after Foundation has loaded -- still makes the class. */
static uintptr_t cf_generic_class_cache;

static uintptr_t cf_generic_owned_class(void)
{
	if (cf_generic_class_cache == 0) {
		os_unfair_lock_lock(&__CFBigRuntimeFunnel);
		if (cf_generic_class_cache == 0) {
			Class super = (Class)objc_getClass("NSObject");

			if (super != NULL) {
				Class cls = objc_allocateClassPair(super, "__NSCFType", 0);

				if (cls != NULL) {
					for (size_t i = 0; cf_object_methods[i].name != NULL; i++) {
						class_addMethod(cls, sel_registerName(cf_object_methods[i].name),
								cf_object_methods[i].imp, cf_object_methods[i].types);
					}
					objc_registerClassPair(cls);
					if (__CFRuntimeOwnedClassCount < __CFRuntimeMaxOwnedClasses) {
						__CFRuntimeOwnedClasses[__CFRuntimeOwnedClassCount++] = (uintptr_t)cls;
					}
					cf_generic_class_cache = (uintptr_t)cls;
				}
			}
		}
		os_unfair_lock_unlock(&__CFBigRuntimeFunnel);
	}
	return cf_generic_class_cache;
}

/* AGORA: IS THIS OBJECT WEARING THE GENERIC CLASS -- a Core Foundation object of a type Foundation never
 * bridged? The generic class inherits ONLY NSObject's methods, so unlike a BRIDGED owned class (__NSCFString
 * IS an NSString) it has no Foundation answer for -hash, -isEqual: or -_copyDescription. The doors below
 * hand an object to Objective-C only when it does, which is what keeps CFHash, CFEqual and CFCopyDescription
 * on Core Foundation's own callbacks for a CTFontDescriptor. */
static Boolean cf_is_generic_object(const void *obj)
{
	return cf_generic_class_cache != 0 && (uintptr_t)object_getClass((void *)obj) == cf_generic_class_cache;
}
#endif

Boolean _CFIsObjC(CFTypeID typeID, void *obj) {
    uintptr_t want, cls;

    if (typeID == _kCFRuntimeNotATypeID || obj == NULL) {
        return false;
    }
    want = __CFISAForTypeID(typeID);
    if (want == 0) {
        return false;	/* this type is not bridged to a class at all */
    }
#if defined(__OBJC__)
    /* A CORE FOUNDATION OBJECT IS NOT A BRIDGED OBJECT, even though it is a subclass of one. Its methods
     * ARE Core Foundation's, so dispatching to them would be Core Foundation asking itself -- the
     * recursion that would follow from -length calling CFStringGetLength. */
    /* BOTH HALVES, AND THE MUTABLE ONE IS NOT AN OPTIMISATION. With the mutable class in place but this
     * refusal still comparing against ONE table, -count on a Core Foundation mutable array dispatched back
     * into the class that answers it and RECURSED. MEASURED: 2618 nested calls on one object, then SIGSEGV.
     * A type's mutable class is as much Core Foundation's own as its immutable one. */
    if ((__CFRuntimeCFOwnClassTable[typeID] != 0
	 && (uintptr_t)object_getClass((void *)obj) == __CFRuntimeCFOwnClassTable[typeID])
	|| (__CFRuntimeCFOwnMutableClassTable[typeID] != 0
	    && (uintptr_t)object_getClass((void *)obj) == __CFRuntimeCFOwnMutableClassTable[typeID])) {
        return false;
    }
#endif
    /* **AND THE WALK NEEDS THE GUARD `_CFIsAnObject` ALREADY HAS, WHICH IT DID NOT HAVE.** `object_getClass`
     * answers A POINTER for anything with an isa — a bare `CFSTR`'s isa is clang's zeroed placeholder storage —
     * and `class_getSuperclass` on that is not a no-op: MEASURED, it SEGFAULTS IN `objc_resolve_class`. That is
     * the shape of the whole defect, and it is the reason this file's other door exists in the shape it does:
     * this function is asked WHETHER AN OBJECT IS OBJECTIVE-C'S and it asks the runtime about an object the
     * runtime does not own.
     *
     *     #0 objc_resolve_class                              libobjc
     *     #1 class_getSuperclass                             libobjc
     *     #2 _CFIsObjC                                       libagora-core   <- here
     *     #3 _CFStrGetSpecialCaseHandlingLanguageIdentifierForLocale
     *     #4 CFStringCompareWithOptionsAndLocale             libagora-core
     *     #5 ct_match_compare                                libagora-text   (the family-name sort)
     *
     * `class_getName` IS SAFE ON SUCH A POINTER AND ANSWERS NULL — `_CFIsAnObject`'s own comment records that
     * measurement, and it is why that function is written this way. So the walk starts only from a class the
     * runtime knows, and an object whose isa the runtime does not know is simply not an object. */
    cls = (uintptr_t)object_getClass(obj);
    if (cls == 0 || class_getName((void *)cls) == NULL) {
        return false;
    }
    for (; cls != 0; cls = (uintptr_t)class_getSuperclass((void *)cls)) {
        if (cls == want) {
            return true;
        }
    }
    return false;
}

CFTypeID _CFRuntimeRegisterClass(const CFRuntimeClass * const cls) {
    // NOTE: If you are adding a type to CF itself, please use a constant value (see CFRuntime_Internal.h)
// className must be pure ASCII string, non-null
    if ((cls->version & _kCFRuntimeCustomRefCount) && !cls->refcount) {
       CFLog(kCFLogLevelWarning, CFSTR("*** _CFRuntimeRegisterClass() given inconsistent class '%s'.  Program will crash soon."), cls->className);
       return _kCFRuntimeNotATypeID;
    }
    os_unfair_lock_lock_with_options(&__CFBigRuntimeFunnel, OS_UNFAIR_LOCK_DATA_SYNCHRONIZATION);
    if (__CFMaxRuntimeTypes <= __CFRuntimeClassTableCount) {
	CFLog(kCFLogLevelWarning, CFSTR("*** CoreFoundation class table full; registration failing for class '%s'.  Program will crash soon."), cls->className);
        os_unfair_lock_unlock(&__CFBigRuntimeFunnel);
	return _kCFRuntimeNotATypeID;
    }
    if (__CFRuntimeClassTableSize <= __CFRuntimeClassTableCount) {
	CFLog(kCFLogLevelWarning, CFSTR("*** CoreFoundation class table full; registration failing for class '%s'.  Program will crash soon."), cls->className);
        os_unfair_lock_unlock(&__CFBigRuntimeFunnel);
	return _kCFRuntimeNotATypeID;
    }
    __CFRuntimeClassTable[__CFRuntimeClassTableCount++] = (CFRuntimeClass *)cls;
    CFTypeID typeID = __CFRuntimeClassTableCount - 1;
    os_unfair_lock_unlock(&__CFBigRuntimeFunnel);
    return typeID;
}

#if defined(__OBJC__)
/* Defined below; the registration above needs it. */
static uintptr_t cf_bridged_subclass(CFTypeID typeID, const void *cls_ref);
#endif

void _CFRuntimeBridgeTypeToClass(CFTypeID cf_typeID, const void *cls_ref) {
    os_unfair_lock_lock(&__CFBigRuntimeFunnel);
    __CFRuntimeObjCClassTable[cf_typeID] = (uintptr_t)cls_ref;
#if defined(__OBJC__)
    /* AND CORE FOUNDATION MAKES ITS OWN CLASS FROM IT -- see cf_bridged_subclass below. Both go in the
     * same lock, because a registration that installed the class but not Core Foundation's own would be
     * a window in which a created object wears the wrong one. */
    if (cls_ref != NULL && __CFRuntimeCFOwnClassTable[cf_typeID] == 0) {
        __CFRuntimeCFOwnClassTable[cf_typeID] = cf_bridged_subclass(cf_typeID, cls_ref);
    }
#endif
    os_unfair_lock_unlock(&__CFBigRuntimeFunnel);
}

const CFRuntimeClass * _CFRuntimeGetClassWithTypeID(CFTypeID typeID) {
    return __CFRuntimeClassTable[typeID]; // hopelessly unthreadsafe
}

void _CFRuntimeUnregisterClassWithTypeID(CFTypeID typeID) {
    os_unfair_lock_lock_with_options(&__CFBigRuntimeFunnel, OS_UNFAIR_LOCK_DATA_SYNCHRONIZATION);
    __CFRuntimeClassTable[typeID] = NULL;
    os_unfair_lock_unlock(&__CFBigRuntimeFunnel);
}


#if defined(DEBUG) || defined(ENABLE_ZOMBIES)

uint8_t __CFZombieEnabled = 0;
uint8_t __CFDeallocateZombies = 0;

extern void __CFZombifyNSObject(void);  // from NSObject.m

void _CFEnableZombies(void) {
}

#endif /* DEBUG */

/*
 Layout of CF info field
 
  3      2       1
  1      4       6        7      0
  |      |       |        |      |
  rrrrrrrrCDX?ttttttttttttaIIIIIII
 
 r = retain count
 C = custom RC
 D = deallocating
 X = deallocated
 t = type ID (12 bits, although only 10 are used because the class table size is 1024)
 a = if set, use system default allocator
 I = type-specific info bits (6 bits available)
 
 On 64 bit, also includes 32 bits more of retain count from 32-63
 */

#if __CF_BIG_ENDIAN__
#define RC_INCREMENT		(1ULL)
#define RC_CUSTOM_RC_BIT	(0x800000ULL << 32)
#define RC_DEALLOCATING_BIT	(0x400000ULL << 32)
#define RC_DEALLOCATED_BIT	(0x200000ULL << 32)
#else
#define RC_INCREMENT		(1ULL << 32)
#define RC_CUSTOM_RC_BIT	(0x800000ULL)
#define RC_DEALLOCATING_BIT	(0x400000ULL)
#define RC_DEALLOCATED_BIT	(0x200000ULL)
#endif

#if TARGET_RT_64_BIT
#define HIGH_RC_START 32
#define HIGH_RC_END 63
#endif

#define LOW_RC_START 24
#define LOW_RC_END 31
#define TYPE_ID_START 8
#define TYPE_ID_END 17

CF_INLINE CFTypeID __CFTypeIDFromInfo(__CFInfoType info) {
    // yes, 10 bits masked off, though 12 bits are there for the type field; __CFRuntimeClassTableSize is 1024
    return (info & __CFInfoMask(TYPE_ID_END, TYPE_ID_START)) >> TYPE_ID_START;
}

/// Get the retain count from the low 32-bit field (the only one stored inline in 32 bit, and unused except for a marker in 64 bit)
CF_INLINE uint16_t __CFLowRCFromInfo(__CFInfoType info) {
    return __CFBitfieldGetValue(info, LOW_RC_END, LOW_RC_START);
}

#if TARGET_RT_64_BIT
/// Get the retain count from the high 32-bit field (only present in 64 bit)
CF_INLINE uint32_t __CFHighRCFromInfo(__CFInfoType info) {
    return __CFBitfield64GetValue(info, HIGH_RC_END, HIGH_RC_START);
}
#endif

CF_INLINE CFRuntimeBase *_cf_aligned_calloc(size_t align, CFIndex size, const char *className) {
    CFRuntimeBase *memory;
    
#if TARGET_OS_MAC
    memory = malloc_zone_memalign(malloc_default_zone(), align, size);
#elif TARGET_OS_LINUX
    int result = posix_memalign((void **)&memory, /*alignment*/ align, size);
    int error = errno;
    enum { errorStringBufferLength = 64 };
    char errorStringBuffer[errorStringBufferLength] = "unknown error";
    const char *errorStringPointer = errorStringBuffer;
    //The GNU-specific version returns a pointer, which may or may not be the pointer passed in. The XSI version returns int. See strerror(3) on Linux.
#if ! ( (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && ! _GNU_SOURCE )
    errorStringPointer =
#endif
    strerror_r(errno, errorStringBuffer, errorStringBufferLength);
    CFLog(kCFLogLevelWarning, CFSTR("*** _CFRuntimeCreateInstance() tried to allocate an instance of '%s', which requires %zu-byte alignment, but memory could not be so allocated: %s"), className, align, errorStringPointer);
#elif TARGET_OS_WIN32
    CFLog(kCFLogLevelWarning, CFSTR("*** _CFRuntimeCreateInstance() tried to allocate an instance of '%s', which requires %zu-byte alignment, but aligned memory is not supported on this platform"), className, align);
    memory = (CFRuntimeBase *)calloc(1, size);
#else
    CFLog(kCFLogLevelWarning, CFSTR("*** _CFRuntimeCreateInstance() tried to allocate an instance of '%s', which requires %zu-byte alignment, but aligned memory is not supported on this platform"), className, align);
    memory = NULL;
#endif
    
    return memory;
}

CFTypeRef _CFRuntimeCreateInstance(CFAllocatorRef allocator, CFTypeID typeID, CFIndex extraBytes, unsigned char *category) {
#if DEPLOYMENT_RUNTIME_SWIFT
    // Under the Swift runtime, all CFTypeRefs are _NSCFTypes or a toll-free bridged type
    
    extern  void *swift_allocObject(uintptr_t metadata, size_t requiredSize, size_t requiredAlignmentMask);
    uintptr_t isa = __CFRuntimeObjCClassTable[typeID];
    CFIndex size = sizeof(CFRuntimeBase) + extraBytes;
    const CFRuntimeClass *cls = __CFRuntimeClassTable[typeID];

#if !defined(__APPLE__) && (defined(__i686__) || (defined(__arm__) && !defined(__aarch64__)) || defined(_M_IX86) || defined(_M_ARM))
    // Linux and Windows 32-bit targets perform 8-byte alignment by default.
    static const int kDefaultAlignment = 8;
#else
    static const int kDefaultAlignment = 16;
#endif

    // Ensure that we get the alignment correct for various targets.  In the
    // case that we are over-aligned `swift_allocObject` will go through a
    // different allocator to ensure that the pointer is suitably aligned.  When
    // we subsequently release the pointer we do not tag that release to go
    // through the overalign'ed path.  This may result in a cross-domainf free
    // and a resultant heap corruption.
    size_t align = (cls->version & _kCFRuntimeRequiresAlignment) ? cls->requiredAlignment : kDefaultAlignment;
    
    CFRuntimeBase *memory = (CFRuntimeBase *)swift_allocObject(isa, size, align - 1);
    
    // Zero the rest of the memory, starting at cfinfo
    memset(&memory->_cfinfoa, 0, size - (sizeof(memory->_cfisa) + sizeof(memory->_swift_rc)));

    // Set up the cfinfo struct
    uint64_t *cfinfop = (uint64_t *)&(memory->_cfinfoa);
    // The 0x80 means we use the default allocator
    *cfinfop = ((typeID << 8) | (0x80));

    return memory;
#else
    if (__CFRuntimeClassTableSize <= typeID) HALT;
    CFAssert1(typeID != _kCFRuntimeNotATypeID, __kCFLogAssertion, "%s(): Uninitialized type id", __PRETTY_FUNCTION__);
    CFRuntimeClass const *cls = __CFRuntimeClassTable[typeID];
    if (NULL == cls) {
	return NULL;
    }
    if (cls->version & _kCFRuntimeRequiresAlignment) {
        allocator = kCFAllocatorSystemDefault;
    }
    Boolean customRC = !!(cls->version & _kCFRuntimeCustomRefCount);
    if (customRC && !cls->refcount) {
        CFLog(kCFLogLevelWarning, CFSTR("*** _CFRuntimeCreateInstance() found inconsistent class '%s'."), cls->className);
        return NULL;
    }
    CFAllocatorRef realAllocator = (NULL == allocator) ? __CFGetDefaultAllocator() : allocator;
    if (kCFAllocatorNull == realAllocator) {
	return NULL;
    }
    Boolean usesSystemDefaultAllocator = _CFAllocatorIsSystemDefault(realAllocator);
    size_t align = (cls->version & _kCFRuntimeRequiresAlignment) ? cls->requiredAlignment : 16;
    // To preserve 16 byte alignment when using custom allocators, we always place the CFAllocatorRef 16 bytes before the CFType. n.b. we assume sizeof(CFAllocatorRef) < 16
    CFIndex size = sizeof(CFRuntimeBase) + extraBytes + (usesSystemDefaultAllocator ? 0 : 16);
    size = (size + 0xF) & ~0xF;	// CF objects are multiples of 16 in size
    // CFType version 0 objects are unscanned by default since they don't have write-barriers and hard retain their innards
    // CFType version 1 objects are scanned and use hand coded write-barriers to store collectable storage within
    Boolean needsClear = false;
    CFRuntimeBase *memory = NULL;
    if (cls->version & _kCFRuntimeRequiresAlignment) {
        memory = _cf_aligned_calloc(align, size, cls->className);
    } else if (__CFAllocatorRespectsHintZeroWhenAllocating(allocator)) {
        memory = (CFRuntimeBase *)CFAllocatorAllocate(allocator, size, _CFAllocatorHintZeroWhenAllocating);
    } else {
        memory = (CFRuntimeBase *)CFAllocatorAllocate(allocator, size, 0);
        needsClear = true;
    }

    if (NULL == memory) {
	return NULL;
    } else if (needsClear) {
        memset(memory, 0, size);
    }

    if (__CFOASafe && category) {
	__CFSetLastAllocationEventName(memory, (char *)category);
    } else if (__CFOASafe) {
	__CFSetLastAllocationEventName(memory, (char *)cls->className);
    }
    if (!usesSystemDefaultAllocator) {
        // add space to hold allocator ref for non-standard allocators.
        // This means the allocator is 16 bytes before the result. See the line where we added 16 bytes above, when !usesSystemDefaultAllocator
        // This retain is balanced in _CFRelease
	*(CFAllocatorRef *)((char *)memory) = (CFAllocatorRef)CFRetain(realAllocator);
	memory = (CFRuntimeBase *)((char *)memory + 16);
    }
    
    // No need for atomic operations here - memory is currently private to this thread
    uint32_t typeIDMasked = (uint32_t)typeID << 8;
    uint32_t usesDefaultAllocatorMasked = usesSystemDefaultAllocator ? 0x80 : 0x00;
#if TARGET_RT_64_BIT
    if (customRC) {
        // The top 32 bits of the word are all FF
        // The rc bits in the lower 32 are 0xFF
        memory->_cfinfoa = (uint64_t)((0xFFFFFFFFULL << 32) | ((uint32_t)(0xFFu << 24) | RC_CUSTOM_RC_BIT | typeIDMasked | usesDefaultAllocatorMasked));
    } else {
        // The top 32 bits of the word start at 1
        // The rc bits in the lower 32 are 0
        memory->_cfinfoa = (uint64_t)((1ULL << 32) | typeIDMasked | usesDefaultAllocatorMasked);
    }
#else
    if (customRC) {
        // The rc bits in the lower 32 are 0xFF
        memory->_cfinfoa = (uint32_t)((0xFFu << 24) | RC_CUSTOM_RC_BIT | typeIDMasked | usesDefaultAllocatorMasked);
    } else {
        // The rc bits in the lower 32 are 1
        memory->_cfinfoa = (uint32_t)((1 << 24) | typeIDMasked | usesDefaultAllocatorMasked);
    }
#endif
/* AGORA: A CORE FOUNDATION OBJECT WEARS NO CLASS. This was
     *   memory->_cfisa = __CFISAForTypeID(typeID);
     * and that one line is what made a native CF object indistinguishable from a Foundation one once the
     * bridge was registered: every CFArrayCreate() came back with isa == [NSArray class], so CF_IS_OBJC
     * answered YES for CF's OWN array, CFArrayAppendValue dispatched into Foundation, and an immutable
     * NSArray was asked to -addObject:. Measured: CFArrayCreateMutable's isa was 0x7f8587bc5470
     * (NSArray) with cfinfoa 0x100001386, against kCFBooleanTrue's isa 0 and cfinfoa 0x1580.
     *
     * __CFRuntimeObjCClassTable ANSWERS TWO QUESTIONS AND THEY ARE DIFFERENT ONES:
     *   CF_IS_OBJC asks "is this object an instance of the class FOUNDATION bridges this type to?" -- the
     *   cluster root, so @[@1,@2] is recognised as a CFArray;
     *   this assignment asked "what class should CF's OWN object wear?" -- and the answer cannot be
     *   Foundation's, because Foundation's methods do not know CF's storage.
     * Apple answers the second with a CF-owned class (__NSCFArray) whose methods ARE Core Foundation's,
     * and Agora does too: cf_bridged_subclass makes it for a bridged type, and cf_generic_owned_class
     * makes __NSCFType for every type Foundation never bridges. Foundation's objects keep their own isas
     * and are still recognised, by the chain walk in _CFIsObjC. */
#if defined(__OBJC__)
    /* A CORE FOUNDATION OBJECT WEARS CORE FOUNDATION'S CLASS FOR ITS TYPE, and __NSCFType when the type
     * has no class of its own -- see cf_generic_owned_class, which is what an unbridged type (every
     * CoreText font, every CoreGraphics color space) wears so that Foundation can hold one. */
    /* AND THE CATEGORY DECIDES WHICH OF THE TWO CLASSES THIS INSTANCE WEARS. The type id cannot: the two
     * halves share one, and the constructors that pass a category are the ones that made a mutable object.
     * A type with no mutable half ignores the category here. */
    if (category != NULL && __CFRuntimeCFOwnMutableClassTable[typeID] != 0) {
        memory->_cfisa = __CFRuntimeCFOwnMutableClassTable[typeID];
    } else {
        memory->_cfisa = __CFRuntimeCFOwnClassTable[typeID];
    }
    /* THE GENERIC CLASS IS THE FLOOR, NOT A SUBSTITUTE: it is used only when the type has no class of
     * its own, so nothing that already had one changes class. */
    if (memory->_cfisa == 0) {
        memory->_cfisa = cf_generic_owned_class();
    }
#else
    memory->_cfisa = 0;
#endif
    if (NULL != cls->init) {
	(cls->init)(memory);
    }
    CFTypeRef cftype = (CFTypeRef)memory;
#ifdef __clang_analyzer__
    // The analyzer doesn't understand that we've manually set the retain count of this new CFTypeRef to 1.
    CFRetain(cftype);
#endif
    return cftype;
#endif
}

#if DEPLOYMENT_RUNTIME_SWIFT
#else
void _CFRuntimeInitStaticInstance(void *ptr, CFTypeID typeID) {
    CFAssert1(typeID != _kCFRuntimeNotATypeID, __kCFLogAssertion, "%s(): Uninitialized type id", __PRETTY_FUNCTION__);
    if (__CFRuntimeClassTableSize <= typeID) HALT;
    CFRuntimeClass const *cfClass = __CFRuntimeClassTable[typeID];
    Boolean customRC = !!(cfClass->version & _kCFRuntimeCustomRefCount);
    if (customRC) {
        CFLog(kCFLogLevelError, CFSTR("*** Cannot initialize a static instance to a class (%s) with custom ref counting"), cfClass->className);
        return;
    }
    CFRuntimeBase *memory = (CFRuntimeBase *)ptr;
    // No need for atomic operations here - memory is currently private to this thread
    uint32_t typeIDMasked = (uint32_t)typeID << 8;
    uint32_t usesDefaultAllocatorMasked = 0x80;
#if TARGET_RT_64_BIT
    if (customRC) {
        // The top 32 bits of the word are the retain count
        memory->_cfinfoa = (uint64_t)((0xFFFFFFFFULL << 32) | (uint32_t)((0xFF << 24) | RC_CUSTOM_RC_BIT | typeIDMasked | usesDefaultAllocatorMasked));
    } else {
        memory->_cfinfoa = (uint64_t)(typeIDMasked | usesDefaultAllocatorMasked);
    }
#else
    if (customRC) {
        // Retain count is in cfinfo
        memory->_cfinfoa = (uint32_t)((0xFF << 24) | RC_CUSTOM_RC_BIT | typeIDMasked | usesDefaultAllocatorMasked);
    } else {
        memory->_cfinfoa = (uint32_t)(typeIDMasked | usesDefaultAllocatorMasked);
    }
#endif
#if defined(__OBJC__)
    /* AND A STATIC INSTANCE OF AN UNBRIDGED TYPE GETS THE GENERIC CLASS TOO. A BRIDGED one is written in
     * afterwards by cf_bridged_subclass (__CFNullBindOwnedClass, __CFBooleanBindOwnedClass), which
     * overwrites this; an unbridged one has nothing else to give it a class. */
    memory->_cfisa = cf_generic_owned_class();
#else
    memory->_cfisa = 0;
#endif
    if (NULL != cfClass->init) {
       (cfClass->init)(memory);
    }
}
#endif

void _CFRuntimeSetInstanceTypeID(CFTypeRef cf, CFTypeID newTypeID) {
    if (__CFRuntimeClassTableSize <= newTypeID) HALT;
    __CFInfoType info = ((CFRuntimeBase *)cf)->_cfinfoa;
    CFTypeID currTypeID = __CFTypeIDFromInfo(info);
    CFRuntimeClass const *newcfClass = __CFRuntimeClassTable[newTypeID];
    Boolean newCustomRC = (newcfClass->version & _kCFRuntimeCustomRefCount);
    CFRuntimeClass const *currcfClass = __CFRuntimeClassTable[currTypeID];
    Boolean currCustomRC = (currcfClass->version & _kCFRuntimeCustomRefCount);
    if (currCustomRC || (0 != currTypeID && newCustomRC)) {
        CFLog(kCFLogLevelError, CFSTR("*** Cannot change the CFTypeID of a %s to a %s due to custom ref counting"), currcfClass->className, newcfClass->className);
        return;
    }
    // Going from current type ID of 0 to anything is allowed, but if
    // the object has somehow already been retained and the transition
    // is to a class doing custom ref counting, the ref count isn't
    // transferred and there will probably be a crash later when the
    // object is freed too early.
    __CFRuntimeSetValue(cf, TYPE_ID_END, TYPE_ID_START, newTypeID);
}

/* THE SAME RE-STAMPING, ASKED WHICH CATEGORY THE INSTANCE IS. A dictionary and a set are created as
 * CFBasicHash objects and then told which CF type they are; the mutable half of each shares its type id
 * with the immutable half, so the type says nothing and the CATEGORY has to -- exactly as it does at
 * creation for the types that build their own object (see _CFRuntimeCreateInstance). */
CF_PRIVATE void _CFRuntimeSetInstanceTypeIDAndIsaForCategory(CFTypeRef cf, CFTypeID newTypeID, const unsigned char *category) {
    _CFRuntimeSetInstanceTypeID(cf, newTypeID);
#if DEPLOYMENT_RUNTIME_SWIFT
    if (_CFTypeGetClass(cf) != __CFISAForTypeID(newTypeID)) {
        ((CFSwiftRef)cf)->isa = (uintptr_t)__CFISAForTypeID(newTypeID);
    }
#elif defined(__OBJC__)
    /* AGORA: THE isa HALF, WHICH THE NON-SWIFT BUILD NEVER HAD AND WHICH THREE TYPES ARE BUILT ON. A
     * CFDictionary and a CFSet are not created as themselves: they are a CFBasicHash -- pure Core
     * Foundation, no bridge -- and then RETYPED here (CFDictionary.c, CFSet.c, CFBag.c, CFBinaryPList.c
     * all come through this door). _CFRuntimeCreateInstance gave that hashtable the class of a type id
     * that is NOT bridged, which is no class at all, so `[(id)CFDictionaryCreate(...) count]` dereferenced
     * a zero isa -- measured, as the last two SIGSEGVs in the suite.
     *
     * THE OWNED CLASS FOR THE NEW TYPE AND NOT __CFISAForTypeID, for the reason that whole table exists:
     * the bridge class is FOUNDATION's and would make a Core Foundation object wear it. Guarded on the
     * object being CORE FOUNDATION'S OWN -- no class, or one of ours, which now includes __NSCFType --
     * so a Foundation object that is ever retyped keeps its own isa. */
    if (_CFTypeGetClass(cf) == 0 || _CFIsOwnedCFObject(cf)) {
        uintptr_t own = ((category != NULL && __CFRuntimeCFOwnMutableClassTable[newTypeID] != 0)

             ? __CFRuntimeCFOwnMutableClassTable[newTypeID] : __CFRuntimeCFOwnClassTable[newTypeID]);

        if (own == 0) {
            own = cf_generic_owned_class();
        }
        ((CFRuntimeBase *)cf)->_cfisa = own;
    }
#endif
}

CF_PRIVATE void _CFRuntimeSetInstanceTypeIDAndIsa(CFTypeRef cf, CFTypeID newTypeID) {
    _CFRuntimeSetInstanceTypeID(cf, newTypeID);
#if DEPLOYMENT_RUNTIME_SWIFT
    if (_CFTypeGetClass(cf) != __CFISAForTypeID(newTypeID)) {
        ((CFSwiftRef)cf)->isa = (uintptr_t)__CFISAForTypeID(newTypeID);
    }
#elif defined(__OBJC__)
    /* AGORA: THE isa HALF, WHICH THE NON-SWIFT BUILD NEVER HAD AND WHICH THREE TYPES ARE BUILT ON. A
     * CFDictionary and a CFSet are not created as themselves: they are a CFBasicHash -- pure Core
     * Foundation, no bridge -- and then RETYPED here (CFDictionary.c, CFSet.c, CFBag.c, CFBinaryPList.c
     * all come through this door). _CFRuntimeCreateInstance gave that hashtable the class of a type id
     * that is NOT bridged, which is no class at all, so `[(id)CFDictionaryCreate(...) count]` dereferenced
     * a zero isa -- measured, as the last two SIGSEGVs in the suite.
     *
     * THE OWNED CLASS FOR THE NEW TYPE AND NOT __CFISAForTypeID, for the reason that whole table exists:
     * the bridge class is FOUNDATION's and would make a Core Foundation object wear it. Guarded on the
     * object being CORE FOUNDATION'S OWN -- no class, or one of ours, which now includes __NSCFType --
     * so a Foundation object that is ever retyped keeps its own isa. */
    if (_CFTypeGetClass(cf) == 0 || _CFIsOwnedCFObject(cf)) {
        uintptr_t own = __CFRuntimeCFOwnClassTable[newTypeID];

        if (own == 0) {
            own = cf_generic_owned_class();
        }
        ((CFRuntimeBase *)cf)->_cfisa = own;
    }
#endif
}


#if !TARGET_RT_64_BIT
enum {
    __kCFObjectRetainedEvent = 12,
    __kCFObjectReleasedEvent = 13
};

#define NUM_EXTERN_TABLES 1
#define EXTERN_TABLE_IDX(O) 0

// we disguise pointers so that programs like 'leaks' forget about these references
#define DISGUISE(O) (~(uintptr_t)(O))

static struct {
    CFLock_t lock;
    CFBasicHashRef table;
//    uint8_t padding[64 - sizeof(CFBasicHashRef) - sizeof(CFLock_t)];
} __NSRetainCounters[NUM_EXTERN_TABLES];

static uintptr_t __CFDoExternRefOperation(uintptr_t op, id obj) {
    if (nil == obj) HALT;
    uintptr_t idx = EXTERN_TABLE_IDX(obj);
    uintptr_t disguised = DISGUISE(obj);
    CFLock_t *lock = &__NSRetainCounters[idx].lock;
    CFBasicHashRef table = __NSRetainCounters[idx].table;
    uintptr_t count;
    switch (op) {
    case 300:   // increment
    case 350:   // increment, no event
        __CFLock(lock);
	CFBasicHashAddValue(table, disguised, disguised);
        __CFUnlock(lock);
        if (__CFOASafe && op != 350) __CFRecordAllocationEvent(__kCFObjectRetainedEvent, obj, 0, 0, NULL);
        return (uintptr_t)obj;
    case 400:   // decrement
        if (__CFOASafe) __CFRecordAllocationEvent(__kCFObjectReleasedEvent, obj, 0, 0, NULL);
    case 450:   // decrement, no event
        __CFLock(lock);
        count = (uintptr_t)CFBasicHashRemoveValue(table, disguised);
        __CFUnlock(lock);
        return 0 == count;
    case 500:
        __CFLock(lock);
        count = (uintptr_t)CFBasicHashGetCountOfKey(table, disguised);
        __CFUnlock(lock);
        return count;
    }
    return 0;
}
#endif

CF_EXPORT CFTypeID CFNumberGetTypeID(void);

CF_INLINE CFTypeID __CFGenericTypeID_inline(const void *cf);

CFTypeID __CFGenericTypeID(const void *cf) {
    return __CFGenericTypeID_inline(cf);
}

CFTypeID CFTypeGetTypeID(void) {
    return _kCFRuntimeIDCFType;
}

CF_PRIVATE void __CFGenericValidateType_(CFTypeRef cf, CFTypeID type, const char *func) {
#if DEPLOYMENT_RUNTIME_SWIFT
    if (cf && CF_IS_SWIFT(type, (CFSwiftRef)cf)) return;
#endif
    if (cf && CF_IS_OBJC(type, cf)) return;
    /* AGORA: A FOUNDATION OBJECT IS EXEMPT, AND BEFORE THE READ RATHER THAN AFTER IT. The
     * assertion below reads the type word at offset 0 -- the isa for a Foundation object,
     * and for a tagged pointer not an address at all. CORE FOUNDATION'S OWN OBJECTS ARE NOT
     * EXEMPT any more: they wear a CF-owned class too (__NSCFType when their type has none),
     * and _CFIsOwnedCFObject is what tells the two apart, exactly as CFRetain/CFRelease do. */
    if (_CFIsAnObject(cf) && !_CFIsOwnedCFObject(cf)) {
        return;
    }
    CFAssert2((cf != NULL) && (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]) && (_kCFRuntimeIDNotAType != __CFGenericTypeID_inline(cf)) && (_kCFRuntimeIDCFType != __CFGenericTypeID_inline(cf)), __kCFLogAssertion, "%s(): pointer %p is not a CF object", func, cf); \
    CFAssert3(__CFGenericTypeID_inline(cf) == type, __kCFLogAssertion, "%s(): pointer %p is not a %s", func, cf, __CFRuntimeClassTable[type]->className);	\
}

#define __CFGenericAssertIsCF(cf) \
    CFAssert2(cf != NULL && (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]) && (_kCFRuntimeIDNotAType != __CFGenericTypeID_inline(cf)) && (_kCFRuntimeIDCFType != __CFGenericTypeID_inline(cf)), __kCFLogAssertion, "%s(): pointer %p is not a CF object", __PRETTY_FUNCTION__, cf);

#if DEPLOYMENT_RUNTIME_SWIFT

CF_INLINE Boolean CFTYPE_IS_SWIFT(const void *obj) {
    CFTypeID typeID = __CFGenericTypeID_inline(obj);
    return CF_IS_SWIFT(typeID, obj);
}

#define CFTYPE_SWIFT_FUNCDISPATCH0(rettype, obj, fn) \
    if (CFTYPE_IS_SWIFT(obj)) return (rettype)__CFSwiftBridge.fn((CFSwiftRef)obj);

#define CFTYPE_SWIFT_FUNCDISPATCH1(rettype, obj, fn, a1) \
    if (CFTYPE_IS_SWIFT(obj)) return (rettype)__CFSwiftBridge.fn((CFSwiftRef)obj, a1);

#else

#define CFTYPE_IS_SWIFT(obj) (0)
#define CFTYPE_SWIFT_FUNCDISPATCH0(rettype, obj, fn) do {} while(0)
#define CFTYPE_SWIFT_FUNCDISPATCH1(rettype, obj, fn, a1) do {} while (0)

#endif


#define CFTYPE_IS_OBJC(obj) (false)
/* AGORA: THE OTHER HALF OF THE FAMILY. The type-word reads are handled at __CFGenericTypeID_inline,
 * which is enough to IDENTIFY a Foundation object -- but the doors then call through Core Foundation's
 * own callbacks (CFString's copyDebugDesc, CFString's equal, ...), and those read CORE FOUNDATION'S
 * STORAGE out of an object that does not have any. Measured, in that order: CFCopyDescription found
 * CFStringGetTypeID() for an NSString and then died inside __CFStringCopyDescription with rdi no longer
 * the tagged pointer. So the doors whose callbacks are Core Foundation's own hand the object to
 * Objective-C instead, which is Apple's CFTYPE_OBJC_FUNCDISPATCH -- A MESSAGE SEND, WHICH IS WHY THIS
 * TRANSLATION UNIT IS COMPILED AS OBJECTIVE-C (see CMakeLists.txt).
 *
 * `sel` is a MESSAGE TAIL, not a selector: `_copyDescription`, `hash`, `isEqual:, cf1`.
 *
 * AND IT IS THE GENERIC CLASS THE DOORS WITHHOLD, NOT EVERY CLASS OF OURS. `_CFIsAnObject` alone answered
 * "has a named class", which was enough only while a Core Foundation object wore no class at all. Now an
 * unbridged one wears __NSCFType (see cf_generic_owned_class) and inherits ONLY NSObject's methods, so a
 * door that handed it over would hand a CTFontDescriptor to -isEqual: and bypass CF's own equal callback.
 * MEASURED, and it is what this exclusion exists for: CTFontMatchingCopyAvailableDescriptors dedups faces
 * by CFEqual, and with the dispatch in place it collapsed FOUR faces to ONE -- so
 * CTFontCreateCopyWithSymbolicTraits found no bold face and every emphasized UI font answered NULL.
 *
 * A BRIDGED OWNED CLASS IS NOT WITHHELD, AND THAT IS DELIBERATE: __NSCFString IS an NSString, so its
 * -hash and -isEqual: ARE Foundation's and CFHash/CFEqual must reach them. MEASURED, AND THE TWO DO NOT
 * AGREE HERE: CFHash on a CF string answers 37884933194 while Foundation's -hash answers
 * 6298719171094792763, so withholding a bridged owned class would break the toll-free agreement that
 * `reg_equality` asserts. Apple withholds EVERY Core Foundation object because Apple's two hashes agree;
 * Agora's do not, and this dispatch is what holds the bridge together until they do. */
#if defined(__OBJC__)
#define CFTYPE_OBJC_FUNCDISPATCH0(rettype, obj, sel) \
    do { if (_CFIsAnObject(obj) && !cf_is_generic_object(obj)) return (rettype)[(id)(obj) sel]; } while (0)
#define CFTYPE_OBJC_FUNCDISPATCH1(rettype, obj, sel, a1) \
    do { if (_CFIsAnObject(obj) && !cf_is_generic_object(obj)) return (rettype)[(id)(obj) sel (a1)]; } while (0)
#else
#define CFTYPE_OBJC_FUNCDISPATCH0(rettype, obj, sel) do {} while (0)
#define CFTYPE_OBJC_FUNCDISPATCH1(rettype, obj, sel, a1) do {} while (0)
#endif


CFTypeID CFGetTypeID(CFTypeRef cf) {
#if defined(DEBUG)
    if (NULL == cf) { CRSetCrashLogMessage("*** CFGetTypeID() called with NULL ***"); HALT; }
#endif
    CFTYPE_SWIFT_FUNCDISPATCH0(CFTypeID, cf, NSObject._cfTypeID);

    /* AGORA: A FOUNDATION OBJECT ANSWERS ITS BRIDGED TYPE ID, and this must come BEFORE the assertion
     * below, which HALTs on anything that is not a Core Foundation object. */
    {
        CFTypeID bridged = _CFBridgedTypeIDForObject(cf);
        if (bridged != _kCFRuntimeNotATypeID) {
            return bridged;
        }
    }

    __CFGenericAssertIsCF(cf);
    return __CFGenericTypeID_inline(cf);
}

CF_PRIVATE CFTypeID _CFGetNonObjCTypeID(CFTypeRef cf) {
    __CFGenericAssertIsCF(cf);
    return __CFGenericTypeID_inline(cf);
}

static const char *const _CFGetTypeIDDescription(CFTypeID type) {
    if (type < __CFRuntimeClassTableCount &&
        NULL != __CFRuntimeClassTable[type] &&
        _kCFRuntimeIDNotAType != type &&
        _kCFRuntimeIDCFType != type) {
        return __CFRuntimeClassTable[type]->className;
    } else {
        return NULL;
    }
}

__attribute__((cold, noinline, noreturn, not_tail_called))
CF_PRIVATE void _CFAssertMismatchedTypeID(CFTypeID expected, CFTypeID actual) {
    char msg[255];
    const char *const expectedName = _CFGetTypeIDDescription(expected) ?: "<unknown>";
    const char *const actualName = _CFGetTypeIDDescription(actual) ?: "<unknown>";
    snprintf(msg, 255, "Expected typeID %lu (%s) does not match actual typeID %lu (%s)", expected, expectedName, actual, actualName);
    HALT_MSG(msg);
}

/* AGORA: THE ONE PLACE EVERY GENERIC DOOR READS THE TYPE WORD, AND SO THE ONE PLACE THAT HAS TO KNOW
 * ABOUT THE BRIDGE. CFGetTypeID, CFCopyDescription, CFEqual, CFHash and every assertion above reach the
 * type id through here, and each of them independently dereferenced offset 0 -- the isa for a Foundation
 * object, and for a tagged pointer not an address at all. Three of them crashed in turn before this was
 * the place to look: CFGetTypeID, then CFRetain/CFRelease, then CFCopyDescription, all with the same
 * pointer in rdi. Fixing each door was not converging, because the family is seven doors wide.
 *
 * A bridged object answers the type id its class bridges to, which is what Apple's -_cfTypeID returns. */
CF_INLINE CFTypeID __CFGenericTypeID_inline(const void *cf) {
    if (_CFIsAnObject(cf)) {
        CFTypeID bridged = _CFBridgedTypeIDForObject(cf);
        if (bridged != _kCFRuntimeNotATypeID) {
            return bridged;
        }
    }
    return __CFTypeIDFromInfo(atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa)));
}


CFStringRef CFCopyTypeIDDescription(CFTypeID type) {
    CFAssert2((NULL != __CFRuntimeClassTable[type]) && _kCFRuntimeIDNotAType != type && _kCFRuntimeIDCFType != type, __kCFLogAssertion, "%s(): type %lu is not a CF type ID", __PRETTY_FUNCTION__, type);
    return CFStringCreateWithCString(kCFAllocatorSystemDefault, __CFRuntimeClassTable[type]->className, kCFStringEncodingASCII);
}

// Bit 31 (highest bit) in second word of cf instance indicates external ref count

static CFTypeRef _CFRetain(CFTypeRef cf, Boolean tryR);

CFTypeRef _CFNonObjCRetain(CFTypeRef cf) {
    __CFGenericAssertIsCF(cf);
    return _CFRetain(cf, false);
}

extern void *objc_retain(void *obj);	/* not declared by the runtime's headers, in either language */
extern void objc_release(void *obj);	/* not declared by the runtime's headers, in either language */

CFTypeRef CFRetain(CFTypeRef cf) {
    if (NULL == cf) { CRSetCrashLogMessage("*** CFRetain() called with NULL ***"); HALT; }
    /* AGORA: A BRIDGED OBJECT IS RETAINED AS AN OBJECT -- libobjc2's own objc_retain, not _CFRetain,
     * which reads the reference count out of cfinfoa. For a Foundation object that word is its isa, and
     * for a small object it is not an address at all: SIGSEGV in CFRetain with rdi 0xd19766cde000002c.
     * The reference count belongs to whichever side made the object, and a tagged pointer's retain is a
     * no-op either way.
     *
     * AND CORE FOUNDATION'S OWN OBJECTS ARE NOT BRIDGED OBJECTS, though they wear a class and so arrive
     * here looking like ones. MEASURED: libobjc2 puts ITS retain count at ((uintptr_t *)cf)[-1], and for a
     * CFRuntime allocation that word is the malloc chunk header -- 0x41 -> 0x42 under CFRetain -> 0x41
     * under CFRelease, with Core Foundation's own count never read and the object therefore never freed.
     * So the test is _CFIsAnObject AND NOT ours: the class decides, because that is where the two
     * reference counts part company. */
    if (_CFIsAnObject(cf) && !_CFIsOwnedCFObject(cf)) {
        return (CFTypeRef)objc_retain((void *)cf);
    }
    __CFGenericAssertIsCF(cf);
    return _CFRetain(cf, false);
}

CFTypeRef CFAutorelease(CFTypeRef __attribute__((cf_consumed)) cf) {
    if (NULL == cf) { CRSetCrashLogMessage("*** CFAutorelease() called with NULL ***"); HALT; }
    return cf;
}

static void _CFRelease(CFTypeRef CF_RELEASES_ARGUMENT cf);

void _CFNonObjCRelease(CFTypeRef cf) {
    __CFGenericAssertIsCF(cf);
    _CFRelease(cf);
}

void CFRelease(CFTypeRef cf) {
    if (NULL == cf) { CRSetCrashLogMessage("*** CFRelease() called with NULL ***"); HALT; }
    /* AGORA: and released as one, for the same reason as CFRetain above -- and with the same exception
     * for Core Foundation's own objects, which is what stops the malloc chunk header being walked back
     * down and lets Core Foundation actually free what it made. */
    if (_CFIsAnObject(cf) && !_CFIsOwnedCFObject(cf)) {
        objc_release((void *)cf);
        return;
    }
    __CFGenericAssertIsCF(cf);
    _CFRelease(cf);
}


CF_PRIVATE void __CFAllocatorDeallocate(CFTypeRef cf);

CF_PRIVATE const void *__CFStringCollectionCopy(CFAllocatorRef allocator, const void *ptr) {
    if (NULL == ptr) { CRSetCrashLogMessage("*** __CFStringCollectionCopy() called with NULL ***"); HALT; }
    CFStringRef theString = (CFStringRef)ptr;
    CFStringRef result = CFStringCreateCopy((allocator), theString);
    return (const void *)result;
}

extern void CFCollection_non_gc_storage_error(void);

CF_PRIVATE const void *__CFTypeCollectionRetain(CFAllocatorRef allocator, const void *ptr) {
    if (NULL == ptr) { CRSetCrashLogMessage("*** __CFTypeCollectionRetain() called with NULL; likely a collection has been corrupted ***"); HALT; }
    CFTypeRef cf = (CFTypeRef)ptr;
    return CFRetain(cf);
}


CF_PRIVATE void __CFTypeCollectionRelease(CFAllocatorRef allocator, const void *ptr) {
    if (NULL == ptr) { CRSetCrashLogMessage("*** __CFTypeCollectionRelease() called with NULL; likely a collection has been corrupted ***"); HALT; }
    CFTypeRef cf = (CFTypeRef)ptr;
    CFRelease(cf);
}

#if !TARGET_RT_64_BIT
static CFLock_t __CFRuntimeExternRefCountTableLock = CFLockInit;
#endif

#if DEPLOYMENT_RUNTIME_SWIFT && !TARGET_OS_MAC
#else
static uint64_t __CFGetFullRetainCount(CFTypeRef cf) {
    if (NULL == cf) { CRSetCrashLogMessage("*** __CFGetFullRetainCount() called with NULL ***"); HALT; }
#if TARGET_RT_64_BIT
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    uint32_t rc = __CFHighRCFromInfo(info);
    if (0 == rc) {
        return (uint64_t)0x0fffffffffffffffULL;
    }
    return rc;
#else
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    uint32_t lowBits = __CFLowRCFromInfo(info);
    if (0 == lowBits) {
        return (uint64_t)0x0fffffffffffffffULL;
    }
    uint64_t highBits = 0;
    if ((lowBits & 0x80) != 0) {
        highBits = __CFDoExternRefOperation(500, (id)cf);
    }
    uint64_t compositeRC = (lowBits & 0x7f) + (highBits << 6);
    return compositeRC;
#endif
}

CF_PRIVATE Boolean __CFRuntimeIsConstant(CFTypeRef cf) {
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    uint32_t rc;
#if TARGET_RT_64_BIT
    rc = __CFHighRCFromInfo(info);
#else
    rc = __CFLowRCFromInfo(info);
#endif
    return rc == 0;
}

/// This is for use during initialization only.
CF_PRIVATE void __CFRuntimeSetRC(CFTypeRef cf, uint32_t rc) {
    // No real need for atomics or CAS here, memory is private to thread so far
    __CFInfoType info = ((CFRuntimeBase *)cf)->_cfinfoa;
#if TARGET_RT_64_BIT
    __CFBitfield64SetValue(info, HIGH_RC_END, HIGH_RC_START, rc);
#else
    __CFBitfieldSetValue(info, LOW_RC_END, LOW_RC_START, rc);
#endif
    ((CFRuntimeBase *)cf)->_cfinfoa = info;
}

CFIndex CFGetRetainCount(CFTypeRef cf) {
    if (NULL == cf) { CRSetCrashLogMessage("*** CFGetRetainCount() called with NULL ***"); HALT; }
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    if (info & RC_CUSTOM_RC_BIT) { // custom ref counting for object
        CFTypeID typeID = __CFTypeIDFromInfo(info);
        CFRuntimeClass const *cfClass = __CFRuntimeClassTable[typeID];
        uint32_t (*refcount)(intptr_t, CFTypeRef) = cfClass->refcount;
        if (!refcount || !(cfClass->version & _kCFRuntimeCustomRefCount) || __CFLowRCFromInfo(info) != 0xFF) {
            HALT; // bogus object
        }
#if TARGET_RT_64_BIT
        if (__CFHighRCFromInfo(info) != 0xFFFFFFFFU) {
            CRSetCrashLogMessage("Detected bogus CFTypeRef");
            HALT; // bogus object
        }
#endif
        uint32_t rc = refcount(0, cf);
#if TARGET_RT_64_BIT
        return (CFIndex)rc;
#else
        return (rc < LONG_MAX) ? (CFIndex)rc : (CFIndex)LONG_MAX;
#endif
    }
    uint64_t rc = __CFGetFullRetainCount(cf);
    return (rc < (uint64_t)LONG_MAX) ? (CFIndex)rc : (CFIndex)LONG_MAX;
}
#endif

// Even though we no longer support GC, leave implementations of these functions in for compatibility.
CFTypeRef CFMakeCollectable(CFTypeRef cf) {
    return cf;
}

CFTypeRef CFMakeUncollectable(CFTypeRef cf) {
    return cf;
}

Boolean _CFNonObjCEqual(CFTypeRef cf1, CFTypeRef cf2) {
    //cf1 is guaranteed to be non-NULL and non-ObjC, cf2 is unknown
    if (cf1 == cf2) return true;
    if (NULL == cf2) { CRSetCrashLogMessage("*** CFEqual() called with NULL second argument ***"); HALT; }
    CFTYPE_OBJC_FUNCDISPATCH1(Boolean, cf2, isEqual:, cf1);
    CFTYPE_SWIFT_FUNCDISPATCH1(Boolean, cf2, NSObject.isEqual, (CFSwiftRef)cf1);
    __CFGenericAssertIsCF(cf1);
    __CFGenericAssertIsCF(cf2);
    if (__CFGenericTypeID_inline(cf1) != __CFGenericTypeID_inline(cf2)) return false;
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal) {
        return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal(cf1, cf2);
    }
    return false;
}

Boolean CFEqual(CFTypeRef cf1, CFTypeRef cf2) {
    if (NULL == cf1) { CRSetCrashLogMessage("*** CFEqual() called with NULL first argument ***"); HALT; }
    if (cf1 == cf2) return true;
    if (NULL == cf2) { CRSetCrashLogMessage("*** CFEqual() called with NULL second argument ***"); HALT; }
    CFTYPE_OBJC_FUNCDISPATCH1(Boolean, cf1, isEqual:, cf2);
    CFTYPE_OBJC_FUNCDISPATCH1(Boolean, cf2, isEqual:, cf1);
    CFTYPE_SWIFT_FUNCDISPATCH1(Boolean, cf1, NSObject.isEqual, (CFSwiftRef)cf2);
    CFTYPE_SWIFT_FUNCDISPATCH1(Boolean, cf2, NSObject.isEqual, (CFSwiftRef)cf1);
    __CFGenericAssertIsCF(cf1);
    __CFGenericAssertIsCF(cf2);
    if (__CFGenericTypeID_inline(cf1) != __CFGenericTypeID_inline(cf2)) return false;
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf1)]->equal(cf1, cf2);
    }
    return false;
}

CFHashCode _CFNonObjCHash(CFTypeRef cf) {
    __CFGenericAssertIsCF(cf);
    CFHashCode (*hash)(CFTypeRef cf) = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash;
    if (NULL != hash) {
        return hash(cf);
    }
    return (CFHashCode)cf;
}

CFHashCode CFHash(CFTypeRef cf) {
    if (NULL == cf) { CRSetCrashLogMessage("*** CFHash() called with NULL ***"); HALT; }
    CFTYPE_OBJC_FUNCDISPATCH0(CFHashCode, cf, hash);
    CFTYPE_SWIFT_FUNCDISPATCH0(CFHashCode, cf, NSObject.hash);
    __CFGenericAssertIsCF(cf);
    CFHashCode (*hash)(CFTypeRef cf) = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->hash; 
    if (NULL != hash) {
	return hash(cf);
    }
    return (CFHashCode)cf;
}


// definition: produces a normally non-NULL debugging description of the object
CFStringRef CFCopyDescription(CFTypeRef cf) {
    if (NULL == cf) return NULL;
    /* AGORA: RESTORED FROM THE COMMENT ABOVE. CFString's copyDebugDesc reads a __CFString; an NSString has
     * none, and this is where that was measured. _copyDescription is Foundation's -- a Copy-named method
     * returning +1, which is why it is not -description. */
    CFTYPE_OBJC_FUNCDISPATCH0(CFStringRef, cf, _copyDescription);
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyDebugDesc) {
	CFStringRef result = __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyDebugDesc(cf);
	if (NULL != result) return result;
    }
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<%s %p [%p]>"), __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->className, cf, CFGetAllocator(cf));
}

// Definition: if type produces a formatting description, return that string, otherwise NULL
CF_PRIVATE CFStringRef __CFCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    if (NULL == cf) return NULL;
    __CFGenericAssertIsCF(cf);
    if (NULL != __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc) {
	return __CFRuntimeClassTable[__CFGenericTypeID_inline(cf)]->copyFormattingDesc(cf, formatOptions);
    }
    return NULL;
}

extern CFAllocatorRef __CFAllocatorGetAllocator(CFTypeRef);

CFAllocatorRef CFGetAllocator(CFTypeRef cf) {
    if (NULL == cf) return kCFAllocatorSystemDefault;
    if (_kCFRuntimeIDCFAllocator == __CFGenericTypeID_inline(cf)) {
	return __CFAllocatorGetAllocator(cf);
    }
    return __CFGetAllocator(cf);
}


extern CFTypeID CFBinaryHeapGetTypeID(void);
extern CFTypeID CFBitVectorGetTypeID(void);
extern CFTypeID CFTreeGetTypeID(void);
extern CFTypeID CFPlugInInstanceGetTypeID(void);
extern CFTypeID CFStringTokenizerGetTypeID(void);
extern CFTypeID CFStorageGetTypeID(void);
#if TARGET_OS_LINUX || TARGET_OS_BSD || (TARGET_OS_OSX && !DEPLOYMENT_RUNTIME_OBJC)
CF_PRIVATE void __CFTSDInitialize(void);
#endif
#if TARGET_OS_WIN32
// From CFPlatform.c
CF_PRIVATE void __CFTSDWindowsCleanup(void);
#endif

#if TARGET_OS_MAC
_Atomic(uint8_t) __CF_FORK_STATE = 0;
char * __crashreporter_info__ = NULL; // Keep this symbol, since it was exported and other things may be linking against it, like GraphicsServices.framework on iOS
__asm(".desc ___crashreporter_info__, 0x10");

static void __cf_atfork_prepare(void) {
    if (pthread_is_threaded_np()) {
        atomic_fetch_or(&__CF_FORK_STATE, __CF_FORK_STATE_MULTITHREADED_FLAG);
    } else {
        atomic_fetch_and(&__CF_FORK_STATE, ~__CF_FORK_STATE_MULTITHREADED_FLAG);
    }
}

static void __cf_atfork_child(void) {
    // Ideally, child-side atfork handlers should be async-cancel-safe, as fork()
    // is async-cancel-safe and can be called from signal handlers.  See also
    // http://standards.ieee.org/reading/ieee/interp/1003-1c-95_int/pasc-1003.1c-37.html
    // This is not a problem for CF.
    uint8_t const state = atomic_load_explicit(&__CF_FORK_STATE, memory_order_relaxed);
    if (state & __CF_FORK_STATE_CF_USED_FLAG) {
        atomic_fetch_or(&__CF_FORK_STATE, __CF_FORK_STATE_FORKED_FLAG);
#if TARGET_OS_OSX
        if (state & __CF_FORK_STATE_MULTITHREADED_FLAG) {
            CRSetCrashLogMessage2("*** multi-threaded process forked ***");
        } else {
            CRSetCrashLogMessage2("*** single-threaded process forked ***");
        }
#endif
    }
}

CF_PRIVATE void __CF_USED(void) {
    atomic_fetch_or(&__CF_FORK_STATE, __CF_FORK_STATE_CF_USED_FLAG);
}

#define EXEC_WARNING_STRING_1 "The process has forked and you cannot use this CoreFoundation functionality safely. You MUST exec().\n"
#define EXEC_WARNING_STRING_2 "Break on __THE_PROCESS_HAS_FORKED_AND_YOU_CANNOT_USE_THIS_COREFOUNDATION_FUNCTIONALITY___YOU_MUST_EXEC__() to debug.\n"

CF_PRIVATE void __THE_PROCESS_HAS_FORKED_AND_YOU_CANNOT_USE_THIS_COREFOUNDATION_FUNCTIONALITY___YOU_MUST_EXEC__(void) {
    write(2, EXEC_WARNING_STRING_1, sizeof(EXEC_WARNING_STRING_1) - 1);
    write(2, EXEC_WARNING_STRING_2, sizeof(EXEC_WARNING_STRING_2) - 1);
//    HALT;
}
#endif


CF_EXPORT const void *__CFArgStuff;
const void *__CFArgStuff = NULL;
void *__CFAppleLanguages = NULL;

// do not cache CFFIXED_USER_HOME or HOME, there are situations where they can change

static struct {
    const char *name;
    const char *value;
} __CFEnv[] = {
    {"USER", NULL},
#if TARGET_OS_WIN32
    {"HOMEPATH", NULL},
    {"HOMEDRIVE", NULL},
    {"USERNAME", NULL},
#endif
    {"TZFILE", NULL},
    {"TZ", NULL},
    {"NEXT_ROOT", NULL},
    {"DYLD_IMAGE_SUFFIX", NULL},
    {"CFProcessPath", NULL},
    {"CFNETWORK_LIBRARY_PATH", NULL},
    {"CFUUIDVersionNumber", NULL},
    {"CFBundleDisableStringsSharing", NULL},
    {"CFCharacterSetCheckForExpandedSet", NULL},
    {"CF_CHARSET_PATH", NULL},
    {"__CF_USER_TEXT_ENCODING", NULL},
    {"APPLE_FRAMEWORKS_ROOT", NULL},
    {"IPHONE_SIMULATOR_ROOT", NULL},
#if !DEPLOYMENT_RUNTIME_OBJC
    {"HOME", NULL},
    {"XDG_DATA_HOME", NULL},
    {"XDG_CONFIG_HOME", NULL},
    {"XDG_DATA_DIRS", NULL},
    {"XDG_CONFIG_DIRS", NULL},
    {"XDG_CACHE_HOME", NULL},
    {"XDG_RUNTIME_DIR", NULL},
#endif
    {NULL, NULL}, // the last one is for optional "COMMAND_MODE" "legacy", do not use this slot, insert before
};

CF_PRIVATE const char *__CFgetenv(const char *n) {
    for (CFIndex idx = 0; idx < sizeof(__CFEnv) / sizeof(__CFEnv[0]); idx++) {
	if (__CFEnv[idx].name && 0 == strcmp(n, __CFEnv[idx].name)) return __CFEnv[idx].value;
    }
    return getenv(n);
}

CF_PRIVATE const char *__CFgetenvIfNotRestricted(const char *n) {
    if (__CFProcessIsRestricted()) return NULL;     // !!! Assumption being this is faster than checking the env
    return __CFgetenv(n);
}

CF_PRIVATE Boolean __CFProcessIsRestricted(void) {
    return issetugid();
}

#if TARGET_OS_WIN32
#define kNilPthreadT  INVALID_HANDLE_VALUE
#else
#define kNilPthreadT  (_CFThreadRef)0
#endif


// Even though we no longer support GC, we leave this exported symbol to avoid lockstep dependencies.
// Match parity with private header
#if (TARGET_OS_MAC && !TARGET_OS_IPHONE && !__x86_64h__)
CF_EXPORT bool kCFUseCollectableAllocator;
bool kCFUseCollectableAllocator = false;
#endif


Boolean __CFProphylacticAutofsAccess = false;
static Boolean __CFInitializing = 0;
Boolean __CFInitialized = 0;

// move the next 2 lines down into the #if below, and make it static, after Foundation gets off this symbol on other platforms. 
CF_EXPORT _CFThreadRef _CFMainPThread;
_CFThreadRef _CFMainPThread = kNilPthreadT;
#if TARGET_OS_WIN32 || TARGET_OS_LINUX || TARGET_OS_BSD

CF_EXPORT _CFThreadRef _CF_pthread_main_thread_np(void);
_CFThreadRef _CF_pthread_main_thread_np(void) {
    return _CFMainPThread;
}
#define pthread_main_thread_np() _CF_pthread_main_thread_np()

#endif



#if TARGET_OS_LINUX || TARGET_OS_BSD || TARGET_OS_WASI
static void __CFInitialize(void) __attribute__ ((constructor));
#endif
#if TARGET_OS_WIN32
CF_EXPORT
#endif

CF_PRIVATE os_unfair_recursive_lock CFPlugInGlobalDataLock;

#if __HAS_DISPATCH__ && !TARGET_OS_MAC
DISPATCH_EXPORT void libdispatch_init();
#endif

void __CFInitialize(void) {
    if (!__CFInitialized && !__CFInitializing) {
        __CFInitializing = 1;

#if __HAS_DISPATCH__ && !TARGET_OS_MAC
    // libdispatch has to be initialized before CoreFoundation, so to avoid
    // issues with static initializer ordering, we are doing it explicitly.
    libdispatch_init();
#endif

    // This is a no-op on Darwin, but is needed on Linux and Windows.
    _CFPerformDynamicInitOfOSRecursiveLock(&CFPlugInGlobalDataLock);

#if TARGET_OS_WIN32
        if (!pthread_main_np()) HALT;   // CoreFoundation must be initialized on the main thread

        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &_CFMainPThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
#elif _POSIX_THREADS
        // move this next line up into the #if above after Foundation gets off this symbol. Also: <rdar://problem/39622745> Stop using _CFMainPThread
        _CFMainPThread = pthread_self();
#else
#error Dont know how to get the main thread on this platform
#endif

#if TARGET_OS_WIN32
        // Must not call any CF functions
        __CFTSDWindowsInitialize();
#elif TARGET_OS_LINUX || TARGET_OS_BSD || (TARGET_OS_MAC && !DEPLOYMENT_RUNTIME_OBJC)
        __CFTSDInitialize();
#endif
        __CFProphylacticAutofsAccess = true;

        for (CFIndex idx = 0; idx < sizeof(__CFEnv) / sizeof(__CFEnv[0]); idx++) {
            if (__CFEnv[idx].name) {
                char *r = NULL;
                if ((r = getenv(__CFEnv[idx].name))) {
                    __CFEnv[idx].value = r;
                }
            }
        }
        
#if TARGET_OS_MAC
	UInt32 s, r;
	__CFStringGetUserDefaultEncoding(&s, &r); // force the potential setenv to occur early
	pthread_atfork(__cf_atfork_prepare, NULL, __cf_atfork_child);
#endif


#if DEPLOYMENT_RUNTIME_SWIFT        
        extern uintptr_t __CFSwiftGetBaseClass(void);
        
        uintptr_t NSCFType = __CFSwiftGetBaseClass();
        for (CFIndex idx = 1; idx < __CFRuntimeClassTableSize; idx++) {
            __CFRuntimeObjCClassTable[idx] = NSCFType;
        }
#endif
        

#if TARGET_OS_MAC
        {
            CFIndex idx, cnt;
            char **args = *_NSGetArgv();
            cnt = *_NSGetArgc();
            for (idx = 1; idx < cnt - 1; idx++) {
                if (NULL == args[idx]) continue;
                if (0 == strcmp(args[idx], "-AppleLanguages") && args[idx + 1]) {
                    CFIndex length = strlen(args[idx + 1]);
                    __CFAppleLanguages = malloc(length + 1);
                    memmove(__CFAppleLanguages, args[idx + 1], length + 1);
                    break;
                }
            }
        }
#endif


#if !TARGET_RT_64_BIT
	for (CFIndex idx = 0; idx < NUM_EXTERN_TABLES; idx++) {
            CFBasicHashCallbacks callbacks = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	    __NSRetainCounters[idx].table = CFBasicHashCreate(kCFAllocatorSystemDefault, kCFBasicHashHasCounts | kCFBasicHashLinearHashing | kCFBasicHashAggressiveGrowth, &callbacks);
	    CFBasicHashSetCapacity(__NSRetainCounters[idx].table, 40);
	    __NSRetainCounters[idx].lock = CFLockInit;
	}
#endif

        /*** _CFRuntimeCreateInstance() can finally be called generally after this line. ***/

        CFNumberGetTypeID();		// NB: This does other work

        __CFCharacterSetInitialize();
        __CFDateInitialize();
        
#if DEPLOYMENT_RUNTIME_SWIFT
        extern void __CFInitializeSwift(void);
        __CFInitializeSwift();
#endif
        

        {
            CFIndex idx, cnt = 0;
            char **args = NULL;
#if TARGET_OS_MAC
            args = *_NSGetArgv();
            cnt = *_NSGetArgc();
#elif TARGET_OS_WIN32
            wchar_t *commandLine = GetCommandLineW();
            // result is actually pointer to wchar_t *, make sure to account for that below
            args = (char **)CommandLineToArgvW(commandLine, (int *)&cnt);
#endif
            CFIndex count;
            CFStringRef *list, buffer[256];
            list = (cnt <= 256) ? buffer : (CFStringRef *)malloc(cnt * sizeof(CFStringRef));
            for (idx = 0, count = 0; idx < cnt; idx++) {
                if (NULL == args[idx]) continue;
#if TARGET_OS_WIN32
                list[count] = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar *)args[idx], wcslen((wchar_t *)args[idx]));
#else
                list[count] = CFStringCreateWithCString(kCFAllocatorSystemDefault, args[idx], kCFStringEncodingUTF8);
                if (NULL == list[count]) {
                    list[count] = CFStringCreateWithCString(kCFAllocatorSystemDefault, args[idx], kCFStringEncodingISOLatin1);
                    // We CANNOT use the string SystemEncoding here;
                    // Do not argue: it is not initialized yet, but these
                    // arguments MUST be initialized before it is.
                    // We should just ignore the argument if the UTF-8
                    // conversion fails, but out of charity we try once
                    // more with ISO Latin1, a standard unix encoding.
                }
#endif
                if (NULL != list[count]) count++;
            }
            __CFArgStuff = CFArrayCreate(kCFAllocatorSystemDefault, (const void **)list, count, &kCFTypeArrayCallBacks);
            if (list != buffer) free(list);
#if TARGET_OS_WIN32
            LocalFree(args);
#endif
        }

        _CFProcessPath();	// cache this early

        __CFOAInitialize();
        

        if (__CFRuntimeClassTableCount < 256) __CFRuntimeClassTableCount = 256;


#if defined(DEBUG) || defined(ENABLE_ZOMBIES)
        const char *value = __CFgetenv("NSZombieEnabled");
        if (value && (*value == 'Y' || *value == 'y')) _CFEnableZombies();
        value = __CFgetenv("NSDeallocateZombies");
        if (value && (*value == 'Y' || *value == 'y')) __CFDeallocateZombies = 0xff;
#endif

#if defined(DEBUG) && TARGET_OS_MAC
        CFLog(kCFLogLevelWarning, CFSTR("Assertions enabled"));
#endif

        __CFProphylacticAutofsAccess = false;

        
        __CFInitializing = 0;
        __CFInitialized = 1;
    }
}

#if TARGET_OS_WIN32

CF_PRIVATE void __CFStringCleanup(void);
CF_PRIVATE void __CFSocketCleanup(void);
CF_PRIVATE void __CFStreamCleanup(void);

static CFBundleRef RegisterCoreFoundationBundle(void) {
    wchar_t path[MAX_PATH+1];
    path[0] = path[1] = 0;
    DWORD wResult;
    CFIndex idx;

    HMODULE ourModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&RegisterCoreFoundationBundle, &ourModule);
    CFAssert(ourModule, __kCFLogAssertion, "GetModuleHandleExW failed");

    wResult = GetModuleFileNameW(ourModule, path, MAX_PATH+1);
    CFAssert1(wResult > 0, __kCFLogAssertion, "GetModuleFileName failed: %d", GetLastError());
    CFAssert1(wResult < MAX_PATH+1, __kCFLogAssertion, "GetModuleFileName result truncated: %s", path);

    // strip off last component, the DLL name
    PathCchRemoveFileSpec(path, wResult);

    CFStringRef fsPath = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (UniChar*)path, wcslen(path));
    CFURLRef dllURL = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, fsPath, kCFURLWindowsPathStyle, TRUE);
    CFURLRef bundleURL = CFURLCreateCopyAppendingPathComponent(kCFAllocatorSystemDefault, dllURL, CFSTR("CoreFoundation.resources"), TRUE);
    CFRelease(fsPath);
    CFRelease(dllURL);

    // this registers us so subsequent calls to CFBundleGetBundleWithIdentifier will succeed
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorSystemDefault, bundleURL);
    CFRelease(bundleURL);

    return bundle;
}


#define DLL_PROCESS_ATTACH   1
#define DLL_THREAD_ATTACH    2
#define DLL_THREAD_DETACH    3
#define DLL_PROCESS_DETACH   0

#if CF_WINDOWS_EXECUTABLE_INITIALIZER
static void __CFWindowsExecutableInitializer(void) __attribute__ ((constructor)) __attribute__ ((used));

void __CFWindowsExecutableInitializer(void) {
    static CFBundleRef cfBundle = NULL;
    __CFInitialize();
    cfBundle = RegisterCoreFoundationBundle();
}
#endif

int DllMain( HINSTANCE hInstance, DWORD dwReason, LPVOID pReserved ) {
    static CFBundleRef cfBundle = NULL;
    if (dwReason == DLL_PROCESS_ATTACH) {
	__CFInitialize();
        cfBundle = RegisterCoreFoundationBundle();
    } else if (dwReason == DLL_PROCESS_DETACH && pReserved == 0) {
        // Only cleanup if we are being unloaded dynamically (pReserved == 0) <rdar://problem/7480873>
        __CFStreamCleanup();
        __CFSocketCleanup();

#if TARGET_OS_WIN32
        // No CF functions should access TSD after this is called
        __CFTSDWindowsCleanup();
#endif

	// do these last
	if (cfBundle) CFRelease(cfBundle);
        __CFStringCleanup();
    }
    return TRUE;
}

#endif

#if DEPLOYMENT_RUNTIME_SWIFT
extern void *swift_retain(void *);
#endif

// For "tryR==true", a return of NULL means "failed".
static CFTypeRef _CFRetain(CFTypeRef cf, Boolean tryR) {
#if DEPLOYMENT_RUNTIME_SWIFT
    // We always call through to swift_retain, since all CFTypeRefs are at least _NSCFType objects
    swift_retain((void *)cf);
    return cf;
#else
    // It's important to load a 64-bit value from cfinfo when running in 64 bit - if we only fetch 32 bits then it's possible we did not atomically fetch the deallocating/deallocated flag and the retain count together (19256102). Therefore it is after this load that we check the deallocating/deallocated flag and the const-ness.
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    if (info & RC_CUSTOM_RC_BIT) {
        if (tryR) return NULL;
        CFTypeID typeID = __CFTypeIDFromInfo(info);
        CFRuntimeClass const *cfClass = __CFRuntimeClassTable[typeID];
        uint32_t (*refcount)(intptr_t, CFTypeRef) = cfClass->refcount;
        if (!refcount || !(cfClass->version & _kCFRuntimeCustomRefCount) || __CFLowRCFromInfo(info) != 0xFF) {
            CRSetCrashLogMessage("Detected bogus CFTypeRef");
            HALT; // bogus object
        }
#if TARGET_RT_64_BIT
        // Custom RC always has high bits all set
        if (__CFHighRCFromInfo(info) != 0xFFFFFFFFU) {
            CRSetCrashLogMessage("Detected bogus CFTypeRef");
            HALT; // bogus object
        }
#endif
        refcount(+1, cf);
    } else {
#if TARGET_RT_64_BIT
        __CFInfoType newInfo = info;
        do {
            if (__builtin_expect(tryR && (info & (RC_DEALLOCATING_BIT | RC_DEALLOCATED_BIT)), false)) {
                // This object is marked for deallocation
                return NULL;
            }
            
            if (__CFHighRCFromInfo(info) == 0) {
                // Constant CFTypeRef
                return cf;
            }
            
            if (__builtin_expect(__CFHighRCFromInfo(info) == ~0U, false)) {
                // Overflow will occur upon add. Turn into constant CFTypeRef (rc == 0). Retain will do nothing, but neither will release.
                newInfo = info;
                __CFBitfield64SetValue(newInfo, HIGH_RC_END, HIGH_RC_START, 0);
            } else {
                // Increment the retain count and swap into place
                newInfo = info + RC_INCREMENT;
            }
        } while (!atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo));
#else
        CFIndex rc = __CFLowRCFromInfo(info);
        if (__builtin_expect(0 == rc, 0)) return cf;    // Constant CFTypeRef
        bool success = 0;
        do {
            // if already deallocating, don't allow new retain
            if (tryR && (info & RC_DEALLOCATING_BIT)) return NULL;
            __CFInfoType newInfo = info;
            newInfo += (1 << LOW_RC_START);
            rc = __CFLowRCFromInfo(newInfo);
            if (__builtin_expect((rc & 0x7f) == 0, 0)) {
                /* Roll over another bit to the external ref count
                 Real ref count = low 7 bits of retain count in info  + external ref count << 6
                 Bit 8 of low bits indicates that external ref count is in use.
                 External ref count is shifted by 6 rather than 7 so that we can set the low
                 bits to to 1100 0000 rather than 1000 0000.
                 This prevents needing to access the external ref count for successive retains and releases
                 when the composite retain count is right around a multiple of 1 << 7.
                 */
                newInfo = info;
                __CFBitfieldSetValue(newInfo, LOW_RC_END, LOW_RC_START, ((1 << 7) | (1 << 6)));
                __CFLock(&__CFRuntimeExternRefCountTableLock);
                success = atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo);
                if (__builtin_expect(success, 1)) {
                    __CFDoExternRefOperation(350, (id)cf);
                }
                __CFUnlock(&__CFRuntimeExternRefCountTableLock);
            } else {
                success = atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo);
            }
        } while (__builtin_expect(!success, 0));
#endif
    }
    if (__builtin_expect(__CFOASafe, 0)) {
	__CFRecordAllocationEvent(__kCFRetainEvent, (void *)cf, 0, CFGetRetainCount(cf), NULL);
    }
    return cf;
#endif
}

#if DEPLOYMENT_RUNTIME_SWIFT
#else
// Never called under GC, only called via ARR weak subsystem; a return of NULL is failure
CFTypeRef _CFTryRetain(CFTypeRef cf) {
    if (NULL == cf) return NULL;
#if OBJC_HAVE_TAGGED_POINTERS
    if (_objc_isTaggedPointer(cf)) return cf; // success
#endif
    return _CFRetain(cf, true);
}

Boolean _CFIsDeallocating(CFTypeRef cf) {
    if (NULL == cf) return false;
#if OBJC_HAVE_TAGGED_POINTERS
    if (_objc_isTaggedPointer(cf)) return false;
#endif
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    if (info & RC_CUSTOM_RC_BIT) {
        return true;   // lie for now; weak references to these objects cannot be formed
    }
    return (info & RC_DEALLOCATING_BIT) ? true : false;
}
#endif

#if DEPLOYMENT_RUNTIME_SWIFT
extern void swift_release(void *);
#endif

static void _CFRelease(CFTypeRef CF_RELEASES_ARGUMENT cf) {
#if DEPLOYMENT_RUNTIME_SWIFT
    // We always call through to swift_release, since all CFTypeRefs are at least _NSCFType objects
    swift_release((void *)cf);
#else
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    CFTypeID typeID = __CFTypeIDFromInfo(info);
    if (info & RC_DEALLOCATED_BIT) {
        char msg[256];

        // Extra checking of values here because we're already in memory-corruption land
        if (typeID < __CFRuntimeClassTableSize) {
            CFRuntimeClass const *cfClass = __CFRuntimeClassTable[typeID];
            snprintf(msg, 256, "Detected over-release of a CFTypeRef %p (%lu / %s)", cf, typeID, cfClass ? cfClass->className : "unknown");
        } else {
            snprintf(msg, 256, "Detected over-release of a CFTypeRef %p (unknown type)", cf);
        }
        CRSetCrashLogMessage(msg);
        HALT;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    CFIndex start_rc = __builtin_expect(__CFOASafe, 0) ? CFGetRetainCount(cf) : 0;
#pragma GCC diagnostic pop
    Boolean isAllocator = (_kCFRuntimeIDCFAllocator == typeID);

    if (info & RC_CUSTOM_RC_BIT) { // custom ref counting for object
        CFRuntimeClass const *cfClass = __CFRuntimeClassTable[typeID];
        uint32_t (*refcount)(intptr_t, CFTypeRef) = cfClass->refcount;
        if (!refcount || !(cfClass->version & _kCFRuntimeCustomRefCount) || __CFLowRCFromInfo(info) != 0xFF) {
            CRSetCrashLogMessage("Detected bogus CFTypeRef");
            HALT; // bogus object
        }
#if TARGET_RT_64_BIT
        if (__CFHighRCFromInfo(info) != 0xFFFFFFFFU) {
            CRSetCrashLogMessage("Detected bogus CFTypeRef");
            HALT; // bogus object
        }
#endif
        refcount(-1, cf);
    } else {
#if TARGET_RT_64_BIT
        uint32_t rc;
        __CFInfoType newInfo;
    again:;
        do {
            rc = __CFHighRCFromInfo(info);
            if (0 == rc) {
                return;        // Constant CFTypeRef
            }
            if (1 == rc) {
                CFRuntimeClass const *cfClass = __CFRuntimeClassTable[typeID];
                if ((cfClass->version & _kCFRuntimeResourcefulObject) && cfClass->reclaim != NULL) {
                    cfClass->reclaim(cf);
                }
                newInfo = info | RC_DEALLOCATING_BIT;
                if (!atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo)) {
                    goto again;
                }
                void (*func)(CFTypeRef) = __CFRuntimeClassTable[typeID]->finalize;
                if (NULL != func) {
                    func(cf);
                }
                // Any further ref-count changes after this point are operating on a finalized object
                // Re-load in case the finalizer call changed the ref count (blech!)
                info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
                rc = __CFHighRCFromInfo(info);
                if (isAllocator || (1 == rc)) {
                    do {
                        // hammer until it takes; trying to retain the object on another thread at this point? too late!
                        newInfo = (info | RC_DEALLOCATED_BIT) - RC_INCREMENT;
                    } while (!atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo));
                    goto really_free;
                }
                
                // drop the deallocating bit; racey, but this resurrection stuff isn't thread-safe anyway
                do {
                    info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
                    newInfo = info & ~RC_DEALLOCATING_BIT;
                } while (!atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo));
                goto again; // still need to have the effect of a CFRelease
            }
            newInfo = info - RC_INCREMENT;
        } while (!atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo));
#else
    again:;
        CFIndex rc = __CFLowRCFromInfo(info);
        if (0 == rc) {
            return;        // Constant CFTypeRef
        }
        bool success = 0;
        Boolean whack = false;
        do {
            rc = __CFLowRCFromInfo(info);
            if (1 == rc) {
                // we think cf should be deallocated
                __CFInfoType newInfo = info | RC_DEALLOCATING_BIT;
                success = atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo);
                if (success) whack = true;
            } else {
                // not yet junk
                __CFInfoType newInfo = info;
                if (rc == (1 << 7)) {
                    // Time to remove a bit from the external ref count
                    __CFLock(&__CFRuntimeExternRefCountTableLock);
                    CFIndex rcHighBitsCnt = __CFDoExternRefOperation(500, (id)cf);
                    if (1 == rcHighBitsCnt) {
                        __CFBitfieldSetValue(newInfo, LOW_RC_END, LOW_RC_START, (1 << 6) - 1);
                    } else {
                        __CFBitfieldSetValue(newInfo, LOW_RC_END, LOW_RC_START, ((1 << 6) | (1 << 7)) - 1);
                    }
                    success = atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo);
                    if (success) {
                        __CFDoExternRefOperation(450, (id)cf);
                    }
                    __CFUnlock(&__CFRuntimeExternRefCountTableLock);
                } else {
                    newInfo -= (1 << LOW_RC_START);
                    success = atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo);
                }
            }
        } while (!success);
        
        if (whack) {
            CFRuntimeClass const *cfClass = __CFRuntimeClassTable[typeID];
            if ((cfClass->version & _kCFRuntimeResourcefulObject) && cfClass->reclaim != NULL) {
                cfClass->reclaim(cf);
            }
            
            if (isAllocator) {
                goto really_free;
            } else {
                void (*func)(CFTypeRef) = __CFRuntimeClassTable[typeID]->finalize;
                if (NULL != func) {
                    func(cf);
                }
                // Any further ref-count changes after this point are operating on a finalized object
                info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
                __CFInfoType newInfo;
                rc = __CFLowRCFromInfo(info);
                if (1 == rc) {
                    do {
                        // hammer until it takes; trying to retain the object on another thread at this point? too late!
                        newInfo = info | RC_DEALLOCATED_BIT;
                    } while (!atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo));
                    goto really_free;
                }
                
                // drop the deallocating bit; racey, but this resurrection stuff isn't thread-safe anyway
                do {
                    newInfo = info & ~(RC_DEALLOCATING_BIT);
                } while (!atomic_compare_exchange_strong(&(((CFRuntimeBase *)cf)->_cfinfoa), &info, newInfo));
                goto again;
            }
        }
#endif
    }
    if (__builtin_expect(__CFOASafe, 0)) {
	__CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, start_rc - 1, NULL);
    }
    return;

    really_free:;
    if (__builtin_expect(__CFOASafe, 0)) {
	// do not use CFGetRetainCount() because cf has been freed if it was an allocator
	__CFRecordAllocationEvent(__kCFReleaseEvent, (void *)cf, 0, 0, NULL);
    }
    // cannot zombify allocators, which get deallocated by __CFAllocatorDeallocate (finalize)
    if (isAllocator) {
        __CFAllocatorDeallocate((void *)cf);
    } else {
	CFAllocatorRef allocator = kCFAllocatorSystemDefault;
        CFAllocatorRef allocatorToRelease = NULL;
	Boolean usesSystemDefaultAllocator = true;

	if (!__CFRuntimeGetFlag(cf, 7)) {
            allocator = CFGetAllocator(cf);
            usesSystemDefaultAllocator = _CFAllocatorIsSystemDefault(allocator);

            /* Agora: upstream references an undefined __kCFAllocatorTypeID_CONST here
               (the non-Swift path is not built on Linux upstream); the allocator type id
               is _kCFRuntimeIDCFAllocator. */
            if (_kCFRuntimeIDCFAllocator != __CFGenericTypeID_inline(cf)) {
                allocatorToRelease = (CFAllocatorRef _Nonnull)__CFGetAllocator(cf);
            }
	}

	{
            // To preserve 16 byte alignment when using custom allocators, we always place the CFAllocatorRef 16 bytes before the CFType. Here we need to make sure we pass the original pointer back to the allocator for deallocating (which included the space for holding the pointer to the allocator itself).
	    CFAllocatorDeallocate(allocator, (uint8_t *)cf - (usesSystemDefaultAllocator ? 0 : 16));
	}

        // This release is balanced in _CFRuntimeCreateInstance, where we retain the allocator used. The analyzer doesn't understand this bit.
#ifndef __clang_analyzer__
	if (allocatorToRelease) {
            CFRelease(allocatorToRelease);
        }
#endif
    }
#endif
}


#if DEPLOYMENT_RUNTIME_SWIFT
struct _CFSwiftBridge __CFSwiftBridge = { { NULL } };

struct _NSCFXMLBridgeStrong __NSCFXMLBridgeStrong = {
  CFArrayGetCount,
  CFArrayGetValueAtIndex,
  CFErrorCreate,
  CFStringCreateWithCString,
  CFStringCreateMutable,
  CFStringAppend,
  CFStringAppendCString,
  CFStringGetLength,
  CFStringGetMaximumSizeForEncoding,
  CFStringGetCString,
  CFDataCreateWithBytesNoCopy,
  CFRelease,
  CFStringCreateWithBytes,
  CFArrayCreateMutable,
  CFArrayAppendValue,
  CFDataGetLength,
  CFDataGetBytePtr,
  CFDictionaryCreateMutable,
  CFDictionarySetValue,
  &kCFAllocatorSystemDefault,
  &kCFAllocatorNull,
  &kCFCopyStringDictionaryKeyCallBacks,
  &kCFTypeDictionaryValueCallBacks,
  &kCFErrorLocalizedDescriptionKey,
};

struct _NSCFXMLBridgeUntyped __NSCFXMLBridgeUntyped = {
  CFArrayGetCount,
  CFArrayGetValueAtIndex,
  CFErrorCreate,
  CFStringCreateWithCString,
  CFStringCreateMutable,
  CFStringAppend,
  CFStringAppendCString,
  CFStringGetLength,
  CFStringGetMaximumSizeForEncoding,
  CFStringGetCString,
  CFDataCreateWithBytesNoCopy,
  CFRelease,
  CFStringCreateWithBytes,
  CFArrayCreateMutable,
  CFArrayAppendValue,
  CFDataGetLength,
  CFDataGetBytePtr,
  CFDictionaryCreateMutable,
  CFDictionarySetValue,
    // We cannot use the real types here because eventually it winds up exposed as API using CF types in Swift, which we do not want
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types-discards-qualifiers"
  &kCFAllocatorSystemDefault,
  &kCFAllocatorNull,
  &kCFCopyStringDictionaryKeyCallBacks,
  &kCFTypeDictionaryValueCallBacks,
  &kCFErrorLocalizedDescriptionKey,
#pragma GCC diagnostic pop
};

// Call out to the CF-level finalizer, because the object is going to go away.
CF_CROSS_PLATFORM_EXPORT void _CFDeinit(CFTypeRef cf) {
    __CFInfoType info = atomic_load(&(((CFRuntimeBase *)cf)->_cfinfoa));
    CFTypeID typeID = __CFTypeIDFromInfo(info);
    void (*func)(CFTypeRef) = __CFRuntimeClassTable[typeID]->finalize;
    if (NULL != func) {
        func(cf);
    }
}

bool _CFIsSwift(CFTypeID type, CFSwiftRef obj) {
    if (type == _kCFRuntimeNotATypeID) {
        return false;
    }
    if (obj->isa == (uintptr_t)__CFConstantStringClassReferencePtr) return false;
    return obj->isa != __CFRuntimeObjCClassTable[type];
}

const char *_NSPrintForDebugger(void *cf) {
    if (CF_IS_SWIFT(_kCFRuntimeNotATypeID, cf)) {
        return "Not a CF Type";
    } else {
        CFStringRef desc = CFCopyDescription((CFTypeRef)cf);
        if (!desc) {
            return "<no description>";
        }
        const char *cheapResult = CFStringGetCStringPtr((CFTypeRef)cf, kCFStringEncodingUTF8);
        if (cheapResult) {
            return cheapResult;
        }
        
        CFIndex bufferSize = 0;
        CFIndex numberConverted = CFStringGetBytes((CFStringRef)cf, CFRangeMake(0, CFStringGetLength((CFStringRef)cf)), kCFStringEncodingUTF8, 0, false, NULL, 0, &bufferSize);
        const char *result = malloc(bufferSize);
        if (!result) {
            return "<unable to fetch description>";
        }
        CFStringGetBytes((CFStringRef)cf, CFRangeMake(0, CFStringGetLength((CFStringRef)cf)), kCFStringEncodingUTF8, 0, false, (UInt8 *)result, bufferSize, NULL);
        // Yes, this leaks
        return result;
    }
}
        
CFHashCode __CFHashDouble(double d) {
    return _CFHashDouble(d);
}

void * _Nullable _CFSwiftRetain(void *_Nullable t) {
    if (t != NULL) {
        swift_retain((void *)t);
        return t;
    } else {
        return NULL;
    }
}

void _CFSwiftRelease(void *_Nullable t) {
    if (t != NULL) {
        swift_release(t);
    }
}

#endif

#undef __CFGenericAssertIsCF


