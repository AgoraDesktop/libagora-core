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
/* NSTimer: CFRunLoop.c. */
- (CFAbsoluteTime)_cffireTime;
/* NSTimer: CFRunLoop.c. */
- (CFTimeInterval)timeInterval;
/* NSInputStream and NSOutputStream: CFStream.c, answering the stream error struct. */
- (CFStreamError)_cfStreamError;
@end

#endif /* __OBJC__ */

#endif /* COREFOUNDATION_CFBRIDGEDMETHODS_H */
