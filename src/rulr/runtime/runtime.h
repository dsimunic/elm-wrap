#ifndef MINI_DATALOG_RUNTIME_H
#define MINI_DATALOG_RUNTIME_H

#include "common/types.h"
#include <stdbool.h>

typedef struct {
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

typedef struct HashEntry {
    long key;
    IntVector rows;
    struct HashEntry *next;
} HashEntry;

typedef struct {
    HashEntry **buckets;
    int         num_buckets;
} HashIndex;

typedef struct {
    TupleBuffer base;
    TupleBuffer delta;
    TupleBuffer next;
} Relation;

typedef struct {
    Relation rel;
    HashIndex idx_on_arg0;
    bool index_enabled;
    int  arity;
    int  stratum;
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
int relation_next_insert_unique(PredRuntime *pr, const Tuple *t);
int relation_base_insert_unique(PredRuntime *pr, const Tuple *t);
int relation_prepare_delta_from_base(PredRuntime *pr);
int relation_promote_next(PredRuntime *pr);

#endif /* MINI_DATALOG_RUNTIME_H */
