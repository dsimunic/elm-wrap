#ifndef MINI_DATALOG_RUNTIME_H
#define MINI_DATALOG_RUNTIME_H

#include "common/types.h"
#include <stdbool.h>
#include <stdint.h>

/* Forward declaration for tuple interning (full definition in tuple_intern.h) */
struct TupleInternTable;

/* Forward declaration for relation providers (full definition in rel_provider.h) */
struct RelProvider;

typedef struct Tuple {
    int   arity;
    Value fields[MAX_ARITY];
} Tuple;

typedef struct {
    Tuple *items;
    int    size;
    int    capacity;
} TupleBuffer;

typedef struct {
    int *indices;
    int  count;
    int  capacity;
} IntVector;

/* Open-addressed hash set for fast tuple deduplication */
typedef struct {
    uint64_t *hashes;      /* Array of tuple hashes (0 = empty slot) */
    int      *row_indices; /* Corresponding row indices in buffer */
    int       capacity;    /* Power of 2 */
    int       count;       /* Number of entries */
} TupleHashSet;

typedef struct HashEntry {
    long key;
    IntVector rows;
    struct HashEntry *next;
} HashEntry;

typedef struct {
    HashEntry **buckets;
    int         num_buckets;
    int         entry_count;  /* Number of distinct keys */
} HashIndex;

typedef struct {
    TupleBuffer base;
    TupleBuffer delta;
    TupleBuffer next;
} Relation;

typedef struct {
    Relation rel;
    HashIndex idx_on_arg0;
    HashIndex idx_on_arg1;    /* Secondary index on arg1 (Phase 2.2) */
    TupleHashSet base_set;    /* Membership set for base buffer (O(1) dedup) */
    TupleHashSet next_set;    /* Membership set for next buffer (O(1) dedup) */
    bool index_enabled;       /* True if arg0 index is enabled */
    bool arg1_index_enabled;  /* True if arg1 index is enabled (Phase 2.2) */
    int  arity;
    int  stratum;
    /* Tuple interning support */
    int  pred_id;             /* Predicate ID for tuple interning */
    struct TupleInternTable *intern_table;  /* Global intern table (NULL if not using) */
    /* BYODS relation provider (Phase 2 of true BYODS support) */
    struct RelProvider *provider;  /* NULL = use default explicit storage */
} PredRuntime;

void tuple_buffer_init(TupleBuffer *buf, int initial_capacity);
void tuple_buffer_clear(TupleBuffer *buf);
int  tuple_buffer_append(TupleBuffer *buf, const Tuple *t);
int  tuple_buffer_copy(TupleBuffer *dst, const TupleBuffer *src);

void hash_index_init(HashIndex *idx, int num_buckets);
void hash_index_clear(HashIndex *idx);
IntVector *hash_index_lookup(HashIndex *idx, long key);
int hash_index_add(HashIndex *idx, long key, int row_index);

void relation_init(PredRuntime *pr, int arity);
void relation_clear(PredRuntime *pr);
void relation_enable_arg1_index(PredRuntime *pr);
int relation_next_insert_unique(PredRuntime *pr, const Tuple *t);
int relation_base_insert_unique(PredRuntime *pr, const Tuple *t);
int relation_prepare_delta_from_base(PredRuntime *pr);
int relation_promote_next(PredRuntime *pr);
void relation_ack_provider_delta(PredRuntime *pr);

#endif /* MINI_DATALOG_RUNTIME_H */
