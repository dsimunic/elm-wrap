#ifndef RULR_SYMBOL_INTERN_H
#define RULR_SYMBOL_INTERN_H

#include <stdint.h>

/*
 * Hash-based symbol interning with O(1) amortized lookup.
 *
 * Replaces the O(n) linear search in the original symbol table with
 * FNV-1a hashing and separate chaining for collision resolution.
 */

#define SYM_HASH_INIT_CAPACITY 256

typedef struct SymInternEntry {
    const char *str;              /* Interned string (arena-allocated) */
    uint32_t    hash;             /* Cached hash for rehashing */
    int         id;               /* Stable symbol ID */
    struct SymInternEntry *next;  /* Chain for collisions */
} SymInternEntry;

typedef struct {
    SymInternEntry **buckets;     /* Hash table buckets */
    int              num_buckets; /* Number of buckets (power of 2) */
    int              count;       /* Number of interned symbols */
    char           **id_to_str;   /* Reverse mapping: ID -> string */
    int              id_capacity; /* Capacity of id_to_str array */
} SymbolIntern;

/* Initialize symbol intern table with default capacity */
void symbol_intern_init(SymbolIntern *si);

/* Intern a string, return stable ID (creates if new) */
int symbol_intern(SymbolIntern *si, const char *str);

/* Lookup by ID (for debugging/output), returns NULL if invalid */
const char *symbol_lookup(const SymbolIntern *si, int id);

/* Cleanup (optional with arena allocation) */
void symbol_intern_destroy(SymbolIntern *si);

#endif /* RULR_SYMBOL_INTERN_H */
