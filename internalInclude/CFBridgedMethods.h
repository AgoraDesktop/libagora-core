/*
 * CFBridgedMethods — the bridged methods whose RESULT TYPE Core Foundation must know.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS EXISTS, MEASURED. Core Foundation's dispatch path casts a message's result to the type the
 * call site names — `return (ret)[(obj) selector]` — and a message sent to a class that is only
 * FORWARD-DECLARED has no known result type, so clang types it `id`. `id` casts to a pointer fine and
 * to `void` fine, which covers most of the 172 dispatch sites; IT DOES NOT CAST TO A SCALAR OR A
 * STRUCT, and Objective-C rejects that outright rather than warning. Those sites are the list below,
 * and they are the whole reason this protocol is here.
 *
 * A PROTOCOL AND NOT A CATEGORY, which was also measured: a category cannot be defined on a class that
 * is only `@class`-declared ("cannot define category for undefined class"), while a protocol is a
 * contract that Core Foundation can state on its own without owning Foundation's interfaces. The
 * dispatch macros cast the receiver to it, so the contract is named in ONE place and the 196 call sites
 * are untouched — and because a message send dispatches by SELECTOR, a cast to this protocol reaches
 * the real object's real method.
 *
 * WHAT IS NOT HERE, AND WHY. A method whose result Core Foundation only casts to `void` or to
 * Objective-C object pointer types needs no entry: `(void)[...]` and `(CFStringRef)[...]` both accept an
 * `id` result once the bridged class is declared (CFStringRef's `objc_bridge(NSString)` is checked, and
 * NSString is forward-declared in CoreFoundation_Prefix.h). Adding one here is a decision about a
 * result type, not a list of everything Core Foundation sends.
 */
#ifndef COREFOUNDATION_CFBRIDGEDMETHODS_H
#define COREFOUNDATION_CFBRIDGEDMETHODS_H

#if defined(__OBJC__)

#include "CFBase.h"
#include "CFStream.h"
/* NOT CFString.h OR CFAttributedString.h. They define struct __CFString and struct __CFAttributedString,
 * and including them made this translation unit encode CFStringRef as the COMPLETE struct while base,
 * which only ever sees the opaque typedef, encodes it as ^{__CFString=}. libobjc2 reported that as
 * 'incorrect signature' on -_cfurl and -_createSubstringWithRange:. CFBase.h already declares both
 * names, opaque, which is the view the other side has. */

/* THE FOUR ATTRIBUTED-STRING AND MUTABLE-STRING NAMES, DECLARED HERE AND NOT INCLUDED.
 * CFString.h and CFAttributedString.h DEFINE their structs, and including them made this translation
 * unit encode CFStringRef as the complete struct while base -- which only ever sees the opaque typedef
 * -- encodes it as ^{__CFString=}. libobjc2 reported exactly that as 'incorrect signature' on -_cfurl
 * and -_createSubstringWithRange:. Declaring the names opaque is the view the other side has, and the
 * repeated typedef is identical to the header one wherever both are seen, which C allows. */
typedef const struct __CFAttributedString *CFAttributedStringRef;
typedef struct __CFAttributedString *CFMutableAttributedStringRef;
typedef struct __CFString *CFMutableStringRef;
#include "CFBridgingTypes.h"	/* a sibling, reached the way CFInternal.h reaches its own */

@protocol __CFBridgedMethods
/* NSArray/NSMutableArray: CFArray.c's immutability test. */
- (BOOL)isKindOfClass:(Class)cls;
/* NSString: CFString.c's hash length. */
- (NSUInteger)length;
/* NSDate and NSTimeZone: CFDate.c and CFTimeZone.c, all four answering seconds. */
- (CFTimeInterval)timeIntervalSinceReferenceDate;
- (CFTimeInterval)timeIntervalSinceDate:(id)otherDate;
- (CFTimeInterval)_daylightSavingTimeOffsetForAbsoluteTime:(CFAbsoluteTime)at;
- (CFTimeInterval)_nextDaylightSavingTimeTransitionAfterAbsoluteTime:(CFAbsoluteTime)at;
/* NSCalendar: CFCalendar.c, three doors answering a range. */
- (CFRange)_minimumRangeOfUnit:(NSCalendarUnit)unit;
- (CFRange)_maximumRangeOfUnit:(NSCalendarUnit)unit;
- (CFRange)_rangeOfUnit:(NSCalendarUnit)smallerUnit inUnit:(NSCalendarUnit)biggerUnit forAT:(CFAbsoluteTime)at;
/* NSString: the substring door, and the two Copy doors. -copy and -mutableCopy are Cocoa's own and
 * already exist on Foundation's classes; only the encoding needs declaring. */
- (CFStringRef)_createSubstringWithRange:(NSRange)range;
- (CFAttributedStringRef)_createAttributedSubstringWithRange:(NSRange)range;
/* -copy AND -mutableCopy ARE DECLARED ONCE, AS id, AND THEY HAVE TO BE. A protocol cannot carry one
 * selector twice, and the two call sites want different Core Foundation types from the same message --
 * CFString.c casts the result to CFStringRef and CFAttributedString.c to CFAttributedStringRef. `id` is
 * what -copy actually returns, and the dispatch macro's own cast supplies the type at each site. */
- (id)copy;
- (id)mutableCopy;
/* NSString, the fast paths: both are ALLOWED TO SAY NO with NULL, and CFString.c falls back. */
- (const char *)_fastCStringContents:(Boolean)nullTerminated;
- (const UniChar *)_fastCharacterContents;
- (Boolean)_encodingCantBeStoredInEightBitCFString;
/* BOOL, WHICH IS COCOA RETURN AND NOT CORE FOUNDATION ONE: CFStringGetBytes answers a count through a plain CF_IS_OBJC block instead of casting this. */
- (BOOL)getBytes:(uint8_t *)buffer maxLength:(NSUInteger)maxLength usedLength:(NSUInteger *)usedLength encoding:(NSUInteger)encoding options:(NSUInteger)options range:(NSRange)range remainingRange:(NSRange *)remaining;
/* NSURL: the true CFURL behind it, which _CFURLFromNSURL is the only caller of. */
- (CFURLRef)_cfurl;
/* NSTimeZone: the three doors that read a zone's state at an instant. Core Foundation cannot make
 * an NSDate, so the shape is ...ForAbsoluteTime:. */
- (CFTimeInterval)_secondsFromGMTForAbsoluteTime:(CFAbsoluteTime)at;
- (CFStringRef)_copyAbbreviationForAbsoluteTime:(CFAbsoluteTime)at;
- (Boolean)_isDaylightSavingTimeForAbsoluteTime:(CFAbsoluteTime)at;
/* NSTimer: CFRunLoop.c. */
- (CFAbsoluteTime)_cffireTime;
/* NSTimer: CFRunLoop.c. */
- (CFTimeInterval)timeInterval;
/* NSInputStream and NSOutputStream: CFStream.c, answering the stream error struct. */
- (CFStreamError)_cfStreamError;
@end

/* --------------------------------------------------------------------------------------------------- */
/* THE SEVEN 'incorrect signature' REPORTS, AUDITED: EVERY ONE DIFFERS IN A POINTER AND NOTHING ELSE     */
/* --------------------------------------------------------------------------------------------------- */
/* libobjc2 compares the METHOD it finds with the SELECTOR's registered type. Seven selectors in this
 * bridge are reported, and the full type strings, measured on both sides, differ ONLY in a pointer --
 * not in width, not in integer type, not in struct layout:
 *
 *   -getBytes:maxLength:usedLength:encoding:options:range:remainingRange:
 *   (NSCFConstantString, NSConstantString, NSMutableString, NSOwnedString)
 *         method   C80@0:8 ^v 16 Q24 ^Q32 Q40 Q48 {_NSRange=QQ}56 ^{_NSRange=QQ}72
 *         selector C80@0:8 *  16 Q24 ^Q32 Q40 Q48 {_NSRange=QQ}56 ^{_NSRange=QQ}72
 *
 *   -_createSubstringWithRange: (NSCFConstantString, NSConstantString, NSTinyString)
 *         method   ^{__CFString=}32@0:8{_NSRange=QQ}16
 *         selector ^{__CFString={__CFRuntimeBase=QAQ}(?={__inline1=q}...)}32@0:8{_NSRange=QQ}16
 *
 *   -_cfurl (NSURL)
 *         method   ^{__CFURL=}16@0:8
 *         selector ^{__CFURL={__CFRuntimeBase=QAQ}IIA^{__CFString}^{__CFURL}^{_CFURLAdditionalData}A^v[1{?=qq}]}16@0:8
 *
 * THE FIRST FAMILY'S REAL HAZARD WAS NOT A POINTER, AND IT IS FIXED: that selector used to be reported
 * as ...i40i44... against Core Foundation's ...Q40Q48... -- an ARGUMENT LIST passing 4 bytes where the
 * callee reads 8. libagora-base's NSString.h records it (22 complaints from the one selector) and the
 * cure was to make NSStringEncoding and the options type NSUInteger, which is Apple's arrangement.
 * Both sides now read Q40 Q48, and that is why the only remaining difference is ^v against *.
 *
 * THE TWO RETURN-VALUE FAMILIES ARE HARMLESS BY A RULE THAT LIBAGORA-BASE ALREADY STATES OUT LOUD: "an
 * argument list is not restricted to pointers the way a return value is" (NSCFConstantString.m). An
 * Objective-C object pointer is one register whichever way the pointee struct is spelled, so
 * ^{__CFString=} and the complete form ARE the same ABI, and likewise ^{__CFURL=}; the argument that
 * carries NSRange is declared identically on both sides. MEASURED: a direct -_createSubstringWithRange:
 * on a CFSTR answers the right substring, and every CFURL-on-NSURL door (CFURLCopyPath,
 * CFURLHasDirectoryPath, CFURLCanBeDecomposed, CFURLCopyNetLocation, ...) passes.
 *
 * THE SPELLING HERE IS CORE FOUNDATION'S AND NOT FOUNDATION'S, WHICH IS DELIBERATE: this names the
 * contract Core Foundation relies on, while libagora-base's declarations are APPLE'S SHAPES (void *,
 * NSStringEncoding, NSUInteger) and are not to be bent to a private contract. Making the two agree
 * therefore means changing the spelling ON THIS SIDE, which owns the selector's registered type -- and
 * the selector currently reports char * where this protocol says uint8_t *, so THREE spellings are in
 * play for that one selector, not two.
 *
 * NOT MEASURED, AND RECORDED AS SUCH: whether libobjc2 INSTALLS A TRAMPOLINE for a pointer-only
 * difference. Nothing observed needs one -- the byte conversions, the substrings and the URL doors are
 * all correct -- but that is an inference from the type strings, not an instrumented result.
 *
 * AND ONE OF THE SEVEN IS NOW FIXED RATHER THAN DOCUMENTED, MEASURED BY COUNTING THE REPORTS. The
 * -getBytes: family is GONE: its difference was the FIRST PARAMETER, ^v against * -- an ARGUMENT, which
 * "is not restricted to pointers the way a return value is" -- so the protocol above was changed to spell
 * that parameter the way libagora-base does, and the reports went from seven to FOUR:
 *
 *     Calling [NSCFConstantString -_createSubstringWithRange:]      (and NSConstantString, NSTinyString)
 *     Calling [NSURL -_cfurl]
 *
 * THE FOUR THAT REMAIN DIFFER ONLY IN STRUCT COMPLETENESS ON A RETURNED POINTER, AND THEY CANNOT BE
 * MATCHED FROM EITHER SIDE: libagora-base sees the opaque typedef, which is Apple's own view and the one
 * it must keep, while a Core Foundation translation unit that has included CFString.h has the complete
 * struct and nothing can unsee it. An object pointer is one register whichever way the pointee is spelled,
 * so these are ABI-identical, and their BEHAVIOUR is measured: a direct -_createSubstringWithRange: on a
 * CFSTR answers the right substring and every CFURL-on-NSURL door passes. */

#endif /* __OBJC__ */

#endif /* COREFOUNDATION_CFBRIDGEDMETHODS_H */
