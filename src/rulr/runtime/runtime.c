#include <string.h>
#include <stdbool.h>
#include "runtime/runtime.h"
#include "common/dyn_array.h"
#include "alloc.h"

#define INITIAL_TUPLE_CAPACITY 16
#define INDEX_BUCKET_COUNT 1024

static bool tuple_equal(const Tuple *a, const Tuple *b) {
    if (a->arity != b->arity) {
        return false;
    }
    for (int i = 0; i < a->arity; ++i) {
        if (!value_equal(a->fields[i], b->fields[i])) {
            return false;
        }
    }
    return true;
}

void tuple_buffer_init(TupleBuffer *buf, int initial_capacity) {
    buf->size = 0;
    buf->capacity = initial_capacity > 0 ? initial_capacity : INITIAL_TUPLE_CAPACITY;
    buf->items = (Tuple *)arena_calloc((size_t)buf->capacity, sizeof(Tuple));
}

void tuple_buffer_clear(TupleBuffer *buf) {
    buf->size = 0;
}

int tuple_buffer_append(TupleBuffer *buf, const Tuple *t) {
    if (dynarray_reserve((void **)&buf->items, &buf->capacity, sizeof(Tuple), buf->size + 1) < 0) {
        return -1;
    }
    buf->items[buf->size++] = *t;
    return buf->size - 1;
}

int tuple_buffer_copy(TupleBuffer *dst, const TupleBuffer *src) {
    if (dynarray_reserve((void **)&dst->items, &dst->capacity, sizeof(Tuple), src->size) < 0) {
        return -1;
    }
    memcpy(dst->items, src->items, (size_t)src->size * sizeof(Tuple));
    dst->size = src->size;
    return 0;
}

void hash_index_init(HashIndex *idx, int num_buckets) {
    idx->num_buckets = num_buckets > 0 ? num_buckets : INDEX_BUCKET_COUNT;
    idx->buckets = (HashEntry **)arena_calloc((size_t)idx->num_buckets, sizeof(HashEntry *));
}

void hash_index_clear(HashIndex *idx) {
    if (idx->buckets) {
        memset(idx->buckets, 0, (size_t)idx->num_buckets * sizeof(HashEntry *));
    }
}

static unsigned long hash_long(long key) {
    unsigned long x = (unsigned long)key;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int int_vector_push(IntVector *vec, int value) {
    if (dynarray_reserve((void **)&vec->indices, &vec->capacity, sizeof(int), vec->count + 1) < 0) {
        return -1;
    }
    vec->indices[vec->count++] = value;
    return 0;
}

IntVector *hash_index_lookup(HashIndex *idx, long key) {
    if (!idx->buckets || idx->num_buckets == 0) {
        return NULL;
    }
    unsigned long h = hash_long(key);
    int bucket = (int)(h % (unsigned long)idx->num_buckets);
    HashEntry *entry = idx->buckets[bucket];
    while (entry) {
        if (entry->key == key) {
            return &entry->rows;
        }
        entry = entry->next;
    }
    return NULL;
}

int hash_index_add(HashIndex *idx, long key, int row_index) {
    if (!idx->buckets || idx->num_buckets == 0) {
        return -1;
    }
    unsigned long h = hash_long(key);
    int bucket = (int)(h % (unsigned long)idx->num_buckets);
    HashEntry *entry = idx->buckets[bucket];
    while (entry) {
        if (entry->key == key) {
            return int_vector_push(&entry->rows, row_index);
        }
        entry = entry->next;
    }
    entry = (HashEntry *)arena_calloc(1, sizeof(HashEntry));
    if (!entry) {
        return -1;
    }
    entry->key = key;
    entry->rows.indices = NULL;
    entry->rows.count = 0;
    entry->rows.capacity = 0;
    int_vector_push(&entry->rows, row_index);
    entry->next = idx->buckets[bucket];
    idx->buckets[bucket] = entry;
    return 0;
}

void relation_init(PredRuntime *pr, int arity) {
    pr->arity = arity;
    pr->stratum = 0;
    tuple_buffer_init(&pr->rel.base, INITIAL_TUPLE_CAPACITY);
    tuple_buffer_init(&pr->rel.delta, INITIAL_TUPLE_CAPACITY);
    tuple_buffer_init(&pr->rel.next, INITIAL_TUPLE_CAPACITY);
    pr->index_enabled = arity > 0;
    if (pr->index_enabled) {
        hash_index_init(&pr->idx_on_arg0, INDEX_BUCKET_COUNT);
    } else {
        pr->idx_on_arg0.buckets = NULL;
        pr->idx_on_arg0.num_buckets = 0;
    }
}

void relation_clear(PredRuntime *pr) {
    tuple_buffer_clear(&pr->rel.base);
    tuple_buffer_clear(&pr->rel.delta);
    tuple_buffer_clear(&pr->rel.next);
    if (pr->index_enabled) {
        hash_index_clear(&pr->idx_on_arg0);
    }
}

static int relation_tuple_exists(const Relation *rel, const Tuple *t) {
    for (int i = 0; i < rel->base.size; ++i) {
        if (tuple_equal(&rel->base.items[i], t)) {
            return 1;
        }
    }
    return 0;
}

int relation_base_insert_unique(PredRuntime *pr, const Tuple *t) {
    if (relation_tuple_exists(&pr->rel, t)) {
        return 0;
    }
    int idx = tuple_buffer_append(&pr->rel.base, t);
    if (idx < 0) {
        return -1;
    }
    if (pr->index_enabled) {
        hash_index_add(&pr->idx_on_arg0, t->fields[0].kind == VAL_SYM ? t->fields[0].u.sym : t->fields[0].u.i, idx);
    }
    return 1;
}

int relation_next_insert_unique(PredRuntime *pr, const Tuple *t) {
    if (relation_tuple_exists(&pr->rel, t)) {
        return 0;
    }
    for (int i = 0; i < pr->rel.next.size; ++i) {
        if (tuple_equal(&pr->rel.next.items[i], t)) {
            return 0;
        }
    }
    return tuple_buffer_append(&pr->rel.next, t) >= 0 ? 1 : -1;
}

int relation_prepare_delta_from_base(PredRuntime *pr) {
    return tuple_buffer_copy(&pr->rel.delta, &pr->rel.base);
}

int relation_promote_next(PredRuntime *pr) {
    if (pr->rel.next.size == 0) {
        pr->rel.delta.size = 0;
        return 0;
    }
    for (int i = 0; i < pr->rel.next.size; ++i) {
        Tuple *t = &pr->rel.next.items[i];
        int idx = tuple_buffer_append(&pr->rel.base, t);
        if (idx < 0) {
            return -1;
        }
        if (pr->index_enabled) {
            long key = t->fields[0].kind == VAL_SYM ? (long)t->fields[0].u.sym : t->fields[0].u.i;
            hash_index_add(&pr->idx_on_arg0, key, idx);
        }
    }

    TupleBuffer tmp = pr->rel.delta;
    pr->rel.delta = pr->rel.next;
    pr->rel.next = tmp;
    pr->rel.next.size = 0;
    return 1;
}
