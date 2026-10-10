/*
 * CFBridgingTypes — the Objective-C spellings of Core Foundation's value types.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * WHY CORE FOUNDATION DECLARES THESE AND FOUNDATION DOES NOT. Core Foundation's dispatch path
 * messages bridged Objective-C objects (CFInternal.h), and the messages take values — a range, an
 * index — in COCOA'S SPELLINGS. A STRUCT PASSED BY VALUE ACROSS THE BRIDGE MUST BE ONE TYPE ON BOTH
 * SIDES OF IT, so the definition has to live where both sides can reach it, and only one direction is
 * available: Foundation depends on Core Foundation, never the reverse. `libagora-base`'s
 * NSObjCRuntime.h imports this header and keeps what is genuinely Foundation's — NSNotFound,
 * NSIntegerMax/NSUIntegerMax, NSRangePointer, NSSortOptions, NSComparisonResult.
 *
 * THE TWO INTEGER TYPES ARE THE SAME TYPES FOUNDATION HAD, width and signedness: `signed long` and
 * `unsigned long`, which is also CFIndex's width, so `CFRange` and `NSRange` are the same shape and a
 * range crosses the bridge without conversion.
 *
 * DECLARED ONLY UNDER `__OBJC__`, because these ARE the Objective-C names: a translation unit compiled
 * as C has no use for them and must not be given them.
 */
#ifndef COREFOUNDATION_CFBRIDGINGTYPES_H
#define COREFOUNDATION_CFBRIDGINGTYPES_H

#if defined(__OBJC__)

typedef signed long NSInteger;
typedef unsigned long NSUInteger;

typedef struct _NSRange {
	NSUInteger location;
	NSUInteger length;
} NSRange;

/* RANGES ARE FUNCTIONS HERE, NOT MACROS, which is Foundation's own measurement and is kept: as macros,
 * `NSLocationInRange(2, range)` failed to compile in this toolchain with "expected identifier" pointing
 * into the macro name, while the fully-expanded expression compiled on its own. Functions cannot be
 * mis-expanded, are type-checked, and take the Cocoa spelling. */
static inline NSRange NSMakeRange(NSUInteger location, NSUInteger length)
{
	NSRange range;

	range.location = location;
	range.length = length;
	return range;
}

static inline NSUInteger NSMaxRange(NSRange range)
{
	return range.location + range.length;
}

#endif /* __OBJC__ */

#endif /* COREFOUNDATION_CFBRIDGINGTYPES_H */
