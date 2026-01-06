#ifndef PLURAL_H
#define PLURAL_H

/*
 * plural.h
 *
 * English (Cardinal) plural rules (CLDR-style):
 *   - "one"  -> when the number is exactly 1
 *   - "other"-> all other cases
 */


/*
 * Convenience helpers for common printf patterns
 *
 * Usage examples:
 *   long n = 1;
 *   printf("Created %ld %s\n", n, en_plural_s(n, "file", "files"));
 *   printf("Created %ld file%s\n", n, en_plural_suffix(n));
 */

/* suffix helper for regular english plurals ("" or "s") */
static inline const char *en_plural_suffix(long n) {
    return (n == 1) ? "" : "s";
}

/* generic selector returning singular or plural form */
static inline const char *en_plural(long n, const char *singular, const char *plural) {
    return (n == 1) ? singular : plural;
}

/* Convenience macro wrappers (evaluate n only once since functions are used).
 * Use these in printf-style calls: en_plural_s(n, "file", "files")
 */
#define en_plural_s(n, singular, plural) en_plural((long)(n), (singular), (plural))
#define en_plural_suffix_s(n) en_plural_suffix((long)(n))

#endif /* PLURAL_H */
