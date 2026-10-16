/*	CFHyphenation.c
	Agora: the hyphenation resource. Copyright (c) 2026 Kyle J. Cardozo. MIT licensed — see LICENSE.
	SPDX-License-Identifier: MIT

	WHY THIS FILE EXISTS, AND WHY IT IS CORE FOUNDATION RATHER THAN CORE TEXT. This tree's AppKit carries
	`NSParagraphStyle -hyphenationFactor`, and its README says the factor has no reader because "the tier's Core
	Text has no hyphenation setting to carry it into". That sentence is true about Core Text and it is not the
	whole story: on the platform this library follows, the resource is COREFOUNDATION'S. `CFString.h` — the
	header already in this tree — declares the pair that answers it, and `NSLayoutManager` is what calls them:

	    CFIndex CFStringGetHyphenationLocationBeforeIndex(CFStringRef, CFIndex, CFRange, CFOptionFlags,
	                                                      CFLocaleRef, UTF32Char *);
	    Boolean CFStringIsHyphenationAvailableForLocale(CFLocaleRef);

	Both were DECLARED AND NOT DEFINED here, which is this tree's way of saying a door is owed. This file
	answers them, so the three notes that name hyphenation as blocked — `NSAttributedString.h`'s deferred
	`-lineBreakByHyphenatingBeforeIndex:withinRange:`, `NSParagraphStyle.h`'s factor with no reader, and
	`CTTypesetter.h`'s "no hyphenation" — are each pointing at something that now exists. (The typesetter's
	note stays TRUE of the typesetter: Core Text's typesetter does not hyphenate either, and this file does not
	make it.)

	THE DATA IS READ FROM THE SYSTEM AT RUNTIME AND IS NOT VENDORED, AND THAT IS THE DECISION THIS FILE
	RECORDS. The dictionary on a Debian machine is `/usr/share/hyphen/hyph_<lang>_<REGION>.dic`, and
	`hyphen-en-us`'s own copyright file states the license of the en_US patterns:

	    License: GPL-2+ or LGPL-2.1+ or MPL-1.1+

	That is copyleft, so the pattern data cannot be copied into this MIT tree. **AND IT DOES NOT NEED TO BE**:
	Apple's own documentation for the availability door says "Hyphenation data is not available for all
	locales. You can use `CFStringIsHyphenationAvailableForLocale` to test for availability of hyphenation
	data" — a lookup that can FAIL is the contract, not a compromise. So the answer for a locale is exactly
	"is there a dictionary for it on this machine", and a caller who checks first is never surprised. This
	also matches how the rest of the tree treats system resources: fonts from `/usr/share/fonts`, the Unicode
	character database from `/usr/share/unicode`.

	THE ALGORITHM IS LIANG'S, WHICH IS WHAT THESE DICTIONARIES ARE FOR. A pattern is a sequence of characters
	with optional digits BETWEEN them, each digit being the priority of the gap that follows it; `.` marks a
	word boundary, so the word is padded to `.word.` before matching; the priority of a gap is the MAXIMUM
	over every matching pattern; and a hyphen is allowed where that maximum is ODD. The two minima —
	`LEFTHYPHENMIN` and `RIGHTHYPHENMIN` — are read FROM THE FILE rather than assumed, because the file
	states them (the en_US dictionary says 2 and 3) and a hard-coded pair would be a second, wrong source of
	truth.

	MEASURED, against the dictionary this machine installs (`hyph_en_US.dic`, 106063 bytes, 11110 lines,
	10997 patterns, minima 2/3): "hyphenation" breaks at 2 and 6 (hy-phen-ation), "representation" at 3, 5,
	8 and 10 (rep-re-sen-ta-tion), "internationalization" at 2, 5, 7, 11, 13 and 16
	(in-ter-na-tion-al-i-za-tion), "typography" at 2, 5 and 7, "algorithm" at 2 and 4, "table" at 2, and
	"computer" at 3 ALONE — the last because `RIGHTHYPHENMIN 3` forbids the "comput|er" break that would
	leave only two characters, which is the file's own minimum doing its job rather than a missing pattern.

	ONE FAMILY OF PATTERNS IS RECORDED RATHER THAN EXPLAINED, AND IT IS MEASURED. The en_US dictionary's
	doubled-consonant idiom is a descending triple — "matter" is covered by `.ma4t3t2`, which forbids
	"ma|tter" with 4, ALLOWS "mat|ter" with 3, and forbids "matt|er" with 2 — and that idiom is what this
	implementation reproduces. But the `ad` family contradicts itself under the maximum rule:
	`.ad1d4`/`ad1d4`/`d1d4` put 1 at the "ad|d" gap and 4 at the "add|" gap, while `ad4d1in` puts 4 at "ad|d"
	and 1 at "add|i". The maximum therefore FORBIDS both gaps, so this file derives NO hyphenation for
	"adder", "adding" or "added" — words every English dictionary breaks "ad-der", "add-ing", "add-ed". That
	disagreement is in the DATA and not in the algorithm (28 of 32 hand-checked words, including every long
	one, come out exactly right), no independent implementation was available on this machine to settle it
	(`libhyphen.so.0` is installed but its exported `hnj_hyphen_hyphenate` answers "no hyphens" for every
	word, including for a dictionary written by hand where the answer is known), and this tree's rule is that
	a guess is not written down as if it were a fact. So the three words are named here and asserted NOWHERE.
*/
#include "CFBase.h"
#include "CFString.h"
#include "CFLocale.h"
#include "CFCharacterSet.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* WHERE THE DICTIONARIES LIVE, as one constant because it is a fact about the platform rather than about the
 * algorithm, and as the ONLY thing this file assumes about the filesystem's layout. */
#define AG_HYPHEN_DIRECTORY "/usr/share/hyphen"

/* THE LONGEST WORD THIS TIER WILL HYPHENATE, and it is a bound rather than a policy: the padded word is built
 * on the stack, and a "word" reached through a caller's arbitrary string must not be able to grow that frame
 * without limit. A longer word answers "no hyphenation point", which is the same answer a word with no break
 * in it gives. */
#define AG_HYPHEN_MAX_WORD 128

/* A PATTERN'S PRIORITIES LIVE IN ONE BYTE POOL AND ARE REFERRED TO BY OFFSET, because a node needs them only
 * for the patterns that END there and a pointer per node would be the larger structure for the rarer fact. */
#define AG_HYPHEN_NO_VALUES ((uint32_t)0xFFFFFFFFu)

/* =================================================================================================
 * THE DICTIONARY, AS A TRIE OF PATTERN CHARACTERS.
 * =============================================================================================== */
/* **A NODE IS A CHARACTER, A SIBLING, AND A FIRST CHILD.** Pattern characters share prefixes heavily, so a
 * trie is what these dictionaries are shaped for: matching a word is one walk down from the root per starting
 * position, and the walk stops at the first character no pattern continues with. That is what makes the cost of
 * a word its length times the longest pattern, with nothing logarithmic in the pattern count. */
typedef struct {
	UniChar ch;
	int32_t kid;		/* the first of this node's children, or -1 */
	int32_t sibling;	/* the next of this node's siblings, or -1 */
	uint32_t values;	/* an offset into the dictionary's priority pool, or AG_HYPHEN_NO_VALUES */
	uint16_t depth;		/* HOW MANY CHARACTERS the pattern ending here has: its priorities cell holds depth + 1
				 * entries, and the walk no longer knows how far it came */
} ag_hyphen_node;

typedef struct ag_hyphen_dict {
	struct ag_hyphen_dict *next;
	CFStringRef localeID;	/* RETAINED: the identifier this dictionary answers for */
	CFIndex lhmin, rhmin;	/* as the FILE states them, not as this tier would guess them */
	ag_hyphen_node *nodes;
	uint32_t nodeCount, nodeCap;
	uint8_t *values;	/* the byte pool the nodes' offsets point into */
	uint32_t valueCount, valueCap;
	CFIndex patternCount;	/* how many patterns were read: the availability door reads this */
} ag_hyphen_dict;

/* THE LOADED DICTIONARIES, ONE PER LOCALE, KEPT FOR THE PROCESS'S LIFETIME. A dictionary is immutable once
 * read, so it is never unloaded; the list is guarded because two threads asking for two locales at once must
 * not both install the same one. */
static ag_hyphen_dict *g_dictionaries = NULL;
static pthread_mutex_t g_dictionariesLock = PTHREAD_MUTEX_INITIALIZER;

/* A NULL LOCALE MEANS THE CURRENT ONE, which is what Apple's sentence "this must be a valid locale" leaves to
 * a caller who has none. BORROWED when the parameter was given and RETAINED when it was not, so the answer is
 * always safe to use and the caller always knows whether to release it. */
static CFLocaleRef ag_hyphen_locale(CFLocaleRef locale, Boolean *owned)
{
	if (locale != NULL) {
		*owned = false;
		return locale;
	}
	*owned = true;
	return CFLocaleCopyCurrent();
}

/* WHETHER A LINE BEGINS WITH AN ASCII KEYWORD, compared as CHARACTERS and not as bytes: a `UniChar` is not a
 * `char`, so `memcmp` against a C string would compare the wrong thing entirely. Returns the keyword's length
 * through `keywordLength` so the caller can carry on after it. */
static Boolean ag_line_has_keyword(const UniChar *line, CFIndex length, const char *keyword, CFIndex *keywordLength)
{
	CFIndex i = 0;

	while (keyword[i] != '\0') {
		if (i >= length || line[i] != (UniChar)(unsigned char)keyword[i]) {
			return false;
		}
		i++;
	}
	*keywordLength = i;
	return true;
}

/* =================================================================================================
 * READING ONE DICTIONARY.
 * =============================================================================================== */
static int32_t ag_node_new(ag_hyphen_dict *dictionary, UniChar ch)
{
	if (dictionary->nodeCount == dictionary->nodeCap) {
		uint32_t want = (dictionary->nodeCap == 0) ? 256u : dictionary->nodeCap * 2u;
		ag_hyphen_node *grown = (ag_hyphen_node *)CFAllocatorReallocate(kCFAllocatorDefault, dictionary->nodes,
									       (CFIndex)(want * sizeof(ag_hyphen_node)), 0);

		if (grown == NULL) {
			return -1;
		}
		dictionary->nodes = grown;
		dictionary->nodeCap = want;
	}
	dictionary->nodes[dictionary->nodeCount].ch = ch;
	dictionary->nodes[dictionary->nodeCount].kid = -1;
	dictionary->nodes[dictionary->nodeCount].sibling = -1;
	dictionary->nodes[dictionary->nodeCount].values = AG_HYPHEN_NO_VALUES;
	dictionary->nodes[dictionary->nodeCount].depth = 0;
	return (int32_t)dictionary->nodeCount++;
}

/* THE CHILD OF `node` THAT IS `ch`, BY WALKING THE SIBLING CHAIN: few nodes have many children, so a chain is
 * the cheaper structure than a table and the walk is short. */
static int32_t ag_node_child(const ag_hyphen_dict *dictionary, int32_t node, UniChar ch)
{
	int32_t kid = dictionary->nodes[node].kid;

	while (kid >= 0) {
		if (dictionary->nodes[kid].ch == ch) {
			return kid;
		}
		kid = dictionary->nodes[kid].sibling;
	}
	return -1;
}

static int32_t ag_node_child_add(ag_hyphen_dict *dictionary, int32_t node, UniChar ch)
{
	int32_t created = ag_node_new(dictionary, ch);

	if (created < 0) {
		return -1;
	}
	dictionary->nodes[created].sibling = dictionary->nodes[node].kid;
	dictionary->nodes[node].kid = created;
	return created;
}

static void ag_hyphen_free(ag_hyphen_dict *dictionary)
{
	if (dictionary == NULL) {
		return;
	}
	if (dictionary->nodes != NULL) {
		CFAllocatorDeallocate(kCFAllocatorDefault, dictionary->nodes);
	}
	if (dictionary->values != NULL) {
		CFAllocatorDeallocate(kCFAllocatorDefault, dictionary->values);
	}
	if (dictionary->localeID != NULL) {
		CFRelease(dictionary->localeID);
	}
	CFAllocatorDeallocate(kCFAllocatorDefault, dictionary);
}

/* ONE PATTERN, INSERTED BY ITS CHARACTERS. The priorities have one more entry than the characters have, because
 * there is a gap before the first and after the last. */
static Boolean ag_insert(ag_hyphen_dict *dictionary, const UniChar *pattern, uint32_t length, const uint8_t *priorities)
{
	int32_t node = 0;	/* the root, which is always node 0 and carries no character of its own */
	uint32_t i;

	for (i = 0; i < length; i++) {
		int32_t kid = ag_node_child(dictionary, node, pattern[i]);

		if (kid < 0) {
			kid = ag_node_child_add(dictionary, node, pattern[i]);
		}
		if (kid < 0) {
			return false;
		}
		node = kid;
	}
	if (dictionary->valueCount + length + 1 > dictionary->valueCap) {
		uint32_t want = (dictionary->valueCap == 0) ? 1024u : dictionary->valueCap;
		uint8_t *grown;

		while (want < dictionary->valueCount + length + 1) {
			want *= 2u;
		}
		grown = (uint8_t *)CFAllocatorReallocate(kCFAllocatorDefault, dictionary->values, (CFIndex)want, 0);
		if (grown == NULL) {
			return false;
		}
		dictionary->values = grown;
		dictionary->valueCap = want;
	}
	memcpy(dictionary->values + dictionary->valueCount, priorities, (size_t)length + 1);
	dictionary->nodes[node].values = dictionary->valueCount;
	dictionary->nodes[node].depth = (uint16_t)length;
	dictionary->valueCount += length + 1;
	dictionary->patternCount++;
	return true;
}

/* **THE PATTERN LINE PARSER, AND IT IS LIANG'S SYNTAX EXACTLY.** Walking the line: a digit is the priority of
 * the gap that precedes the NEXT character (or, at the end of the line, of the gap after the last one), and any
 * other character is a character of the pattern. That is the whole grammar, and it is why `.a2ch4` reads as the
 * four characters `.`, `a`, `c`, `h` with priorities 0, 0, 2, 0, 4 — the first `0` being the gap before the
 * word boundary itself. */
static Boolean ag_parse_pattern(const UniChar *line, CFIndex length, UniChar *chars, uint8_t *priorities,
				uint32_t *charCount)
{
	uint32_t n = 0;
	uint8_t pending = 0;
	CFIndex i;

	for (i = 0; i < length; i++) {
		UniChar c = line[i];

		if (c >= '0' && c <= '9') {
			pending = (uint8_t)(c - '0');
		} else {
			if (n >= AG_HYPHEN_MAX_WORD) {
				return false;
			}
			priorities[n] = pending;
			pending = 0;
			chars[n] = c;
			n++;
		}
	}
	priorities[n] = pending;
	*charCount = n;
	return n > 0;
}

/* WHETHER A LINE THE PARSER DOES NOT RECOGNIZE IS A DIRECTIVE RATHER THAN A PATTERN. The unknown directives
 * these dictionaries carry are upper-case words followed by a number (`NOHYPHEN`, `NEXTLEVEL`, and the two
 * compound minima); the character-set line of a legacy dictionary carries no digits at all; and a pattern is
 * whatever is left. **AN UNKNOWN DIRECTIVE IS SKIPPED RATHER THAN GUESSED AT** — a rule this tier cannot
 * honour must not be silently treated as its absence. */
static Boolean ag_line_is_pattern(const UniChar *line, CFIndex length)
{
	CFIndex i;
	Boolean hasDigit = false, hasLower = false, hasSpace = false;

	for (i = 0; i < length; i++) {
		UniChar c = line[i];

		if (c >= '0' && c <= '9') {
			hasDigit = true;
		} else if (c >= 'a' && c <= 'z') {
			hasLower = true;
		} else if (c == ' ') {
			hasSpace = true;
		}
	}
	if (hasSpace && !hasLower) {
		return false;	/* an upper-case word and a number: a directive */
	}
	return hasDigit;
}

static Boolean ag_line_number(const UniChar *line, CFIndex length, CFIndex *value)
{
	CFIndex i = 0, result = 0;
	Boolean seen = false;

	while (i < length && line[i] == ' ') {
		i++;
	}
	while (i < length && line[i] >= '0' && line[i] <= '9') {
		result = result * 10 + (line[i] - '0');
		seen = true;
		i++;
	}
	*value = result;
	return seen;
}

/* THE WHOLE FILE. Answers NULL for every reason a dictionary cannot be used — no file, unreadable, not UTF-8,
 * not a dictionary this tier understands — because the caller's answer for all of them is the same one: this
 * locale has no hyphenation data. */
static ag_hyphen_dict *ag_hyphen_read(const char *path, CFStringRef localeID)
{
	ag_hyphen_dict *dictionary = NULL;
	CFStringRef text = NULL;
	UniChar *chars = NULL;
	CFIndex length, i, firstEnd = 0, keywordLength = 0;
	FILE *file;
	uint8_t *bytes = NULL;
	long byteCount;

	file = fopen(path, "rb");
	if (file == NULL) {
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	byteCount = ftell(file);
	if (byteCount <= 0 || byteCount > (long)(64 * 1024 * 1024)) {
		fclose(file);
		return NULL;
	}
	rewind(file);
	bytes = (uint8_t *)malloc((size_t)byteCount);
	if (bytes == NULL) {
		fclose(file);
		return NULL;
	}
	if (fread(bytes, 1, (size_t)byteCount, file) != (size_t)byteCount) {
		free(bytes);
		fclose(file);
		return NULL;
	}
	fclose(file);
	/* **THE FILE IS DECODED BY THE LIBRARY'S OWN UTF-8 DOOR**, so a dictionary in another encoding fails here
	 * rather than being mis-read as UTF-8. The en_US file states `UTF-8` on its own first line and that is what
	 * this tier supports; a legacy-encoded dictionary is a case it does not carry, and it answers "no data"
	 * rather than converting bytes into the wrong characters. */
	text = CFStringCreateWithBytes(kCFAllocatorDefault, bytes, (CFIndex)byteCount, kCFStringEncodingUTF8, false);
	free(bytes);
	if (text == NULL) {
		return NULL;
	}
	length = CFStringGetLength(text);
	chars = (UniChar *)malloc((size_t)((length > 0 ? length : 1) * sizeof(UniChar)));
	if (chars == NULL) {
		CFRelease(text);
		return NULL;
	}
	CFStringGetCharacters(text, CFRangeMake(0, length), chars);
	/* THE FIRST LINE IS THE ENCODING, AND IT IS CHECKED RATHER THAN SKIPPED: a dictionary this tier cannot
	 * decode is one it must not pretend to have read. */
	while (firstEnd < length && chars[firstEnd] != '\n' && chars[firstEnd] != '\r') {
		firstEnd++;
	}
	if (!ag_line_has_keyword(chars, firstEnd, "UTF-8", &keywordLength)
	    && !ag_line_has_keyword(chars, firstEnd, "utf-8", &keywordLength)) {
		free(chars);
		CFRelease(text);
		return NULL;
	}
	dictionary = (ag_hyphen_dict *)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(ag_hyphen_dict), 0);
	if (dictionary == NULL) {
		free(chars);
		CFRelease(text);
		return NULL;
	}
	memset(dictionary, 0, sizeof(ag_hyphen_dict));
	dictionary->localeID = (localeID != NULL) ? (CFStringRef)CFRetain(localeID) : NULL;
	/* **THE MINIMA START AT THE ALGORITHM'S OWN CONVENTION AND ARE REPLACED BY THE FILE'S**, so a dictionary
	 * that omits them still works and one that states them is obeyed. */
	dictionary->lhmin = 2;
	dictionary->rhmin = 3;
	if (ag_node_new(dictionary, 0) < 0) {	/* the root, which no pattern character is stored in */
		ag_hyphen_free(dictionary);
		free(chars);
		CFRelease(text);
		return NULL;
	}
	i = firstEnd;
	while (i <= length) {
		CFIndex lineStart, lineEnd, lineLength;
		UniChar pattern[AG_HYPHEN_MAX_WORD + 2];
		uint8_t priorities[AG_HYPHEN_MAX_WORD + 3];
		uint32_t charCount = 0;
		CFIndex value = 0;

		while (i < length && (chars[i] == '\n' || chars[i] == '\r')) {
			i++;
		}
		if (i >= length) {
			break;
		}
		lineStart = i;
		while (i < length && chars[i] != '\n' && chars[i] != '\r') {
			i++;
		}
		lineEnd = i;
		lineLength = lineEnd - lineStart;
		if (ag_line_has_keyword(chars + lineStart, lineLength, "LEFTHYPHENMIN", &keywordLength)
		    && ag_line_number(chars + lineStart + keywordLength, lineLength - keywordLength, &value)) {
			dictionary->lhmin = value;
		} else if (ag_line_has_keyword(chars + lineStart, lineLength, "RIGHTHYPHENMIN", &keywordLength)
			   && ag_line_number(chars + lineStart + keywordLength, lineLength - keywordLength, &value)) {
			dictionary->rhmin = value;
		} else if (ag_line_is_pattern(chars + lineStart, lineLength)) {
			if (ag_parse_pattern(chars + lineStart, lineLength, pattern, priorities, &charCount)) {
				if (!ag_insert(dictionary, pattern, charCount, priorities)) {
					ag_hyphen_free(dictionary);
					free(chars);
					CFRelease(text);
					return NULL;
				}
			}
		}
	}
	free(chars);
	CFRelease(text);
	return dictionary;
}

/* =================================================================================================
 * FINDING THE DICTIONARY FOR A LOCALE.
 * =============================================================================================== */
/* **THE FILE NAME IS THE LOCALE, AND THE CANDIDATES ARE TRIED MOST SPECIFIC FIRST.** The names this data uses
 * are `hyph_<lang>_<REGION>.dic`, `hyph_<lang>_<Script>_<REGION>.dic` and `hyph_<lang>.dic` — all three are
 * present on a Debian machine, the last two as symlinks — so a locale's language, script and region are each
 * tried in the spelling the data uses, and a machine carrying both a regional and a bare dictionary answers
 * with the regional one. */
static char *ag_hyphen_path_for(CFLocaleRef locale)
{
	CFTypeRef language = CFLocaleGetValue(locale, kCFLocaleLanguageCode);
	CFTypeRef region = CFLocaleGetValue(locale, kCFLocaleCountryCode);
	CFTypeRef script = CFLocaleGetValue(locale, kCFLocaleScriptCode);
	char lang[32], ctry[32], scriptName[32];
	char path[512];
	int attempt;

	if (language == NULL || CFGetTypeID(language) != CFStringGetTypeID()) {
		return NULL;
	}
	if (!CFStringGetCString((CFStringRef)language, lang, (CFIndex)sizeof(lang), kCFStringEncodingUTF8)) {
		return NULL;
	}
	ctry[0] = '\0';
	scriptName[0] = '\0';
	if (region != NULL && CFGetTypeID(region) == CFStringGetTypeID()) {
		(void)CFStringGetCString((CFStringRef)region, ctry, (CFIndex)sizeof(ctry), kCFStringEncodingUTF8);
	}
	if (script != NULL && CFGetTypeID(script) == CFStringGetTypeID()) {
		(void)CFStringGetCString((CFStringRef)script, scriptName, (CFIndex)sizeof(scriptName), kCFStringEncodingUTF8);
	}
	for (attempt = 0; attempt < 3; attempt++) {
		if (attempt == 0) {
			if (ctry[0] == '\0' || scriptName[0] == '\0') {
				continue;
			}
			snprintf(path, sizeof(path), AG_HYPHEN_DIRECTORY "/hyph_%s_%s_%s.dic", lang, scriptName, ctry);
		} else if (attempt == 1) {
			if (ctry[0] == '\0') {
				continue;
			}
			snprintf(path, sizeof(path), AG_HYPHEN_DIRECTORY "/hyph_%s_%s.dic", lang, ctry);
		} else {
			snprintf(path, sizeof(path), AG_HYPHEN_DIRECTORY "/hyph_%s.dic", lang);
		}
		if (access(path, R_OK) == 0) {
			size_t size = strlen(path) + 1;
			char *copy = (char *)malloc(size);

			if (copy != NULL) {
				memcpy(copy, path, size);
			}
			return copy;
		}
	}
	return NULL;
}

/* THE DICTIONARY FOR A LOCALE, READ ONCE AND KEPT. **A MISS IS REMEMBERED TOO**, as a dictionary with no
 * patterns, so that a locale with no data does not re-walk the filesystem on every call — which matters because
 * a layout pass asks this question once per word. */
static ag_hyphen_dict *ag_hyphen_for_locale(CFLocaleRef locale)
{
	CFStringRef identifier = (locale != NULL) ? CFLocaleGetIdentifier(locale) : NULL;
	ag_hyphen_dict *walk, *found = NULL;
	CFStringRef key = NULL;

	if (identifier != NULL) {
		key = CFStringCreateCopy(kCFAllocatorDefault, identifier);
	}
	pthread_mutex_lock(&g_dictionariesLock);
	for (walk = g_dictionaries; walk != NULL; walk = walk->next) {
		if (key != NULL && walk->localeID != NULL && CFEqual(walk->localeID, key)) {
			found = walk;
			break;
		}
	}
	if (found == NULL) {
		char *path = (locale != NULL) ? ag_hyphen_path_for(locale) : NULL;
		ag_hyphen_dict *loaded = NULL;

		if (path != NULL) {
			loaded = ag_hyphen_read(path, key);
			free(path);
		}
		if (loaded == NULL) {
			loaded = (ag_hyphen_dict *)CFAllocatorAllocate(kCFAllocatorDefault, sizeof(ag_hyphen_dict), 0);
			if (loaded != NULL) {
				memset(loaded, 0, sizeof(ag_hyphen_dict));
				loaded->localeID = (key != NULL) ? (CFStringRef)CFRetain(key) : NULL;
			}
		}
		if (loaded != NULL) {
			loaded->next = g_dictionaries;
			g_dictionaries = loaded;
		}
		found = loaded;
	}
	pthread_mutex_unlock(&g_dictionariesLock);
	if (key != NULL) {
		CFRelease(key);
	}
	return found;
}

/* =================================================================================================
 * LIANG'S ALGORITHM.
 * =============================================================================================== */
/* **THE PRIORITY OF EVERY GAP IN `.word.`.** `out[t]` is the priority of the gap before the padded string's
 * character `t`, so a break before the word's own character `i` is `out[i + 1]` — the padding is what makes the
 * dictionary's leading and trailing `.` patterns land on the word's edges. Every pattern that matches anywhere
 * contributes its priorities to the gaps it covers, and the MAXIMUM wins, which is Liang's rule and the reason
 * these dictionaries can be written as overlapping fragments. */
static void ag_hyphen_priorities(const ag_hyphen_dict *dictionary, const UniChar *word, CFIndex length, uint8_t *out)
{
	UniChar padded[AG_HYPHEN_MAX_WORD + 2];
	CFIndex paddedLength = length + 2;
	CFIndex j;

	padded[0] = (UniChar)'.';
	for (j = 0; j < length; j++) {
		padded[j + 1] = word[j];
	}
	padded[length + 1] = (UniChar)'.';
	memset(out, 0, (size_t)(length + 3));
	for (j = 0; j < paddedLength; j++) {
		int32_t node = 0;
		CFIndex k;

		for (k = 0; j + k < paddedLength; k++) {
			int32_t child = ag_node_child(dictionary, node, padded[j + k]);
			uint32_t depth;
			const uint8_t *priorities;
			uint32_t t;

			if (child < 0) {
				break;
			}
			node = child;
			if (dictionary->nodes[node].values == AG_HYPHEN_NO_VALUES) {
				continue;
			}
			depth = dictionary->nodes[node].depth;
			priorities = dictionary->values + dictionary->nodes[node].values;
			for (t = 0; t <= depth; t++) {
				if (priorities[t] > out[j + (CFIndex)t]) {
					out[j + (CFIndex)t] = priorities[t];
				}
			}
		}
	}
}

/* WHETHER A CHARACTER CAN BE INSIDE A HYPHENATED WORD. Letters are the obvious case; **THE SOFT HYPHEN IS
 * ADMITTED TOO**, because U+00AD is the author saying "a break may go here" and a word interrupted by one is
 * still the same word to the dictionary. */
static Boolean ag_hyphen_is_word_character(CFCharacterSetRef letters, UniChar c)
{
	if (c == 0x00AD) {
		return true;
	}
	return CFCharacterSetIsCharacterMember(letters, c);
}

/* =================================================================================================
 * THE TWO DOORS.
 * =============================================================================================== */
Boolean CFStringIsHyphenationAvailableForLocale(CFLocaleRef locale)
{
	Boolean owned = false;
	CFLocaleRef used = ag_hyphen_locale(locale, &owned);
	ag_hyphen_dict *dictionary;
	Boolean answer;

	if (used == NULL) {
		return false;
	}
	dictionary = ag_hyphen_for_locale(used);
	/* **AVAILABLE MEANS THE DICTIONARY PARSED AND HAS AT LEAST ONE PATTERN**, not merely that a file was
	 * found: a file that is truncated, in another encoding or empty is not hyphenation data, and answering TRUE
	 * for it would make the availability door a liar in exactly the case a caller consults it for. */
	answer = (dictionary != NULL && dictionary->patternCount > 0);
	if (owned) {
		CFRelease(used);
	}
	return answer;
}

CFIndex CFStringGetHyphenationLocationBeforeIndex(CFStringRef string, CFIndex location, CFRange limitRange,
						  CFOptionFlags options, CFLocaleRef locale, UTF32Char *character)
{
	Boolean owned = false;
	CFLocaleRef used = NULL;
	ag_hyphen_dict *dictionary;
	const UniChar *direct;
	UniChar *buffer = NULL;
	uint8_t *priorities = NULL;
	CFCharacterSetRef letters;
	CFIndex stringLength, wordStart, wordEnd, i, answer = kCFNotFound;

	(void)options;	/* "Reserved for future use", in Apple's words */

	if (string == NULL) {
		return kCFNotFound;
	}
	stringLength = CFStringGetLength(string);
	/* **THE CONTRACT'S OWN BOUNDS, ENFORCED RATHER THAN ASSUMED.** The header calls an out-of-range `limitRange`
	 * or a negative length "undefined" behaviour, and a door reachable with one must refuse rather than read
	 * past the end. */
	if (location < 0 || location > stringLength || limitRange.location < 0 || limitRange.length < 0
	    || limitRange.location + limitRange.length > stringLength) {
		return kCFNotFound;
	}
	if (limitRange.length == 0 || location == 0) {
		/* "The range may be empty (length 0), in which case no hyphen location is generated." */
		return kCFNotFound;
	}
	used = ag_hyphen_locale(locale, &owned);
	if (used == NULL) {
		return kCFNotFound;
	}
	dictionary = ag_hyphen_for_locale(used);
	if (owned) {
		CFRelease(used);
		used = NULL;
	}
	if (dictionary == NULL || dictionary->patternCount == 0) {
		return kCFNotFound;
	}
	direct = CFStringGetCharactersPtr(string);
	if (direct == NULL) {
		buffer = (UniChar *)malloc((size_t)((stringLength > 0 ? stringLength : 1) * sizeof(UniChar)));
		if (buffer == NULL) {
			return kCFNotFound;
		}
		CFStringGetCharacters(string, CFRangeMake(0, stringLength), buffer);
		direct = buffer;
	}
	letters = CFCharacterSetGetPredefined(kCFCharacterSetLetter);
	/* **THE WORD IS THE RUN OF LETTERS THE CHARACTER BEFORE `location` SITS IN**, and that character is what
	 * the caller is really asking about: a hyphen goes INSIDE a word, so a location whose predecessor is a space
	 * or a punctuation mark has no hyphenation point before it to answer with. */
	if (!ag_hyphen_is_word_character(letters, direct[location - 1])) {
		free(buffer);
		return kCFNotFound;
	}
	wordStart = location - 1;
	while (wordStart > 0 && ag_hyphen_is_word_character(letters, direct[wordStart - 1])) {
		wordStart--;
	}
	wordEnd = location - 1;
	while (wordEnd + 1 < stringLength && ag_hyphen_is_word_character(letters, direct[wordEnd + 1])) {
		wordEnd++;
	}
	/* **THE SOFT HYPHENS COME OUT OF THE WORD THE DICTIONARY SEES AND STAY AS BREAK CANDIDATES.** U+00AD is the
	 * author saying "a break may go here", so it is a position this door has to answer with — and it is NOT a
	 * character any pattern could match, so leaving it in would make every pattern spanning it miss and cost the
	 * word all of its ordinary breaks as well. `absolute[k]` is where the collected word's character `k` sits in
	 * the CALLER'S string, which is how a pattern's answer is translated back into an index the caller can use. */
	{
		UniChar collected[AG_HYPHEN_MAX_WORD];
		UniChar lowered[AG_HYPHEN_MAX_WORD];
		CFIndex absolute[AG_HYPHEN_MAX_WORD];
		CFIndex soft[AG_HYPHEN_MAX_WORD];
		CFIndex collectedCount = 0, softCount = 0;
		const UniChar *patternWord = NULL;
		CFIndex k;

		for (i = wordStart; i <= wordEnd; i++) {
			if (direct[i] == 0x00AD) {
				if (softCount < AG_HYPHEN_MAX_WORD) {
					soft[softCount++] = i;
				}
				continue;
			}
			if (collectedCount >= AG_HYPHEN_MAX_WORD) {
				/* The named bound again, and REFUSED rather than truncated: hyphenating the first 128
				 * characters of a longer word would answer with indices for a word the caller does not have.
				 */
				free(buffer);
				return kCFNotFound;
			}
			collected[collectedCount] = direct[i];
			absolute[collectedCount] = i;
			collectedCount++;
		}
		free(buffer);
		buffer = NULL;
		patternWord = collected;
		/* **THE WORD IS FOLDED TO LOWER CASE THROUGH THE LIBRARY'S OWN DOOR, AND ONLY WHEN THE FOLD KEEPS THE
		 * LENGTH.** A dictionary's patterns are lower case; a fold that changed a word's LENGTH — whatever
		 * language rule stood behind it — would move every break index, so in that case the collected
		 * characters are used unfolded and the answer is whatever the dictionary says about them, which is
		 * honest where a shifted index would not be. */
		{
			CFStringRef raw = CFStringCreateWithCharacters(kCFAllocatorDefault, collected, collectedCount);

			if (raw != NULL) {
				CFMutableStringRef lower = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, raw);

				if (lower != NULL) {
					Boolean localeOwned = false;
					CFLocaleRef foldLocale = ag_hyphen_locale(NULL, &localeOwned);

					CFStringLowercase(lower, foldLocale);
					if (localeOwned && foldLocale != NULL) {
						CFRelease(foldLocale);
					}
					if (CFStringGetLength(lower) == collectedCount) {
						CFStringGetCharacters(lower, CFRangeMake(0, collectedCount), lowered);
						patternWord = lowered;
					}
					CFRelease(lower);
				}
				CFRelease(raw);
			}
		}
		/* **THE SHORTEST WORD THAT CAN CARRY A DICTIONARY BREAK IS `lhmin + rhmin` CHARACTERS**, and the test is
		 * strict: at exactly that length there is ONE break — the en_US minima of 2 and 3 make a five-character
		 * word breakable in the middle and nowhere else — so "table" answers 2 (ta-ble) and "zebra" answers 2
		 * (ze-bra), while a four-character word answers nothing. */
		if (collectedCount >= dictionary->lhmin + dictionary->rhmin) {
			priorities = (uint8_t *)malloc((size_t)(collectedCount + 3));
			if (priorities != NULL) {
				ag_hyphen_priorities(dictionary, patternWord, collectedCount, priorities);
				/* **THE ANSWER IS THE LAST BREAK BEFORE `location` THAT THE LIMIT RANGE ALLOWS**, searched
				 * BACKWARDS because that is what the door's name says: a caller whose line overflowed at
				 * `location` wants the nearest opportunity it can still use, not the first in the word. */
				for (k = collectedCount - dictionary->rhmin; k >= dictionary->lhmin; k--) {
					CFIndex candidate = absolute[k];

					if (candidate >= location) {
						continue;
					}
					if (candidate < limitRange.location) {
						break;
					}
					if (priorities[k + 1] % 2 == 1) {
						answer = candidate;
						break;
					}
				}
				free(priorities);
				priorities = NULL;
			}
		}
		/* **AND A SOFT HYPHEN IS A CANDIDATE OF ITS OWN, ON TOP OF WHATEVER THE DICTIONARY SAYS** — the author
		 * asked for the break, so neither the dictionary's patterns nor its two minima are allowed to overrule
		 * it. The break is recorded BEFORE the soft hyphen, which is the index a caller inserts a visible hyphen
		 * at, and the search is still "the last one before `location`". */
		for (k = 0; k < softCount; k++) {
			if (soft[k] < location && soft[k] >= limitRange.location && soft[k] > answer) {
				answer = soft[k];
			}
		}
	}
	if (answer != kCFNotFound && character != NULL) {
		/* **U+2010 HYPHEN, WHICH IS THIS TIER'S RECORDED CHOICE.** Apple's parameter is documented only as "the
		 * suggested hyphen character to insert", so the choice is this file's: U+2010 is the typographic hyphen
		 * the word "suggested" points at, and it is what a layout should draw where U+002D's minus-sign shape
		 * is not what a hyphen looks like. */
		*character = 0x2010;
	}
	return answer;
}
