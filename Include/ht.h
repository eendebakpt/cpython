#ifndef HT_H
#define HT_H
// Simple hash table implemented in C.


#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Hash table structure: create with ht_create, free with ht_destroy.
typedef struct ht ht;

// Hash table entry (slot may be filled or empty).
typedef struct {
    const char* key;  // key is NULL if this slot is empty
    void* value;
} ht_entry;

// Hash table structure: create with ht_create, free with ht_destroy.
struct ht {
    ht_entry* entries;  // hash slots
    size_t capacity;    // size of _entries array
    size_t length;      // number of items in hash table
};

#define INITIAL_CAPACITY 16  // must not be zero

inline ht* ht_create(void) {
    // Allocate space for hash table struct.
    ht* table = (ht*)malloc(sizeof(ht));
    if (table == NULL) {
        return NULL;
    }
    table->length = 0;
    table->capacity = INITIAL_CAPACITY;

    // Allocate (zero'd) space for entry buckets.
    table->entries = (ht_entry*) calloc(table->capacity, sizeof(ht_entry));
    if (table->entries == NULL) {
        free(table); // error, free table before we return!
        return NULL;
    }
    return table;
}

inline void ht_destroy(ht* table) {
    // First free allocated keys.
    for (size_t i = 0; i < table->capacity; i++) {
        free((void*)table->entries[i].key);
    }

    // Then free entries array and table itself.
    free(table->entries);
    free(table);
}

#define FNV_OFFSET 14695981039346656037UL
#define FNV_PRIME 1099511628211UL

// Return 64-bit FNV-1a hash for key (NUL-terminated). See description:
// https://en.wikipedia.org/wiki/Fowler–Noll–Vo_hash_function
static uint64_t hash_key(const char* key) {
    uint64_t hash = FNV_OFFSET;
    for (const char* p = key; *p; p++) {
        hash ^= (uint64_t)(unsigned char)(*p);
        hash *= FNV_PRIME;
    }
    return hash;
}

inline void* ht_get(ht* table, const char* key) {
    // AND hash with capacity-1 to ensure it's within entries array.
    uint64_t hash = hash_key(key);
    size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

    // Loop till we find an empty entry.
    while (table->entries[index].key != NULL) {
        if (strcmp(key, table->entries[index].key) == 0) {
            // Found key, return value.
            return table->entries[index].value;
        }
        // Key wasn't in this slot, move to next (linear probing).
        index++;
        if (index >= table->capacity) {
            // At end of entries array, wrap around.
            index = 0;
        }
    }
    return NULL;
}

// Internal function to set an entry (without expanding table).
inline  const char* ht_set_entry(ht_entry* entries, size_t capacity,
        const char* key, void* value, size_t* plength) {
    // AND hash with capacity-1 to ensure it's within entries array.
    uint64_t hash = hash_key(key);
    size_t index = (size_t)(hash & (uint64_t)(capacity - 1));

    // Loop till we find an empty entry.
    while (entries[index].key != NULL) {
        if (strcmp(key, entries[index].key) == 0) {
            // Found key (it already exists), update value.
            entries[index].value = value;
            return entries[index].key;
        }
        // Key wasn't in this slot, move to next (linear probing).
        index++;
        if (index >= capacity) {
            // At end of entries array, wrap around.
            index = 0;
        }
    }

    // Didn't find key, allocate+copy if needed, then insert it.
    if (plength != NULL) {
        key = _strdup(key);
        if (key == NULL) {
            return NULL;
        }
        (*plength)++;
    }
    entries[index].key = (char*)key;
    entries[index].value = value;
    return key;
}

// Expand hash table to twice its current size. Return true on success,
// false if out of memory.
inline  int ht_expand(ht* table) {
    // Allocate new entries array.
    size_t new_capacity = table->capacity * 2;
    if (new_capacity < table->capacity) {
        return 0;  // overflow (capacity would be too big)
    }
    ht_entry* new_entries = (ht_entry*)calloc(new_capacity, sizeof(ht_entry));
    if (new_entries == NULL) {
        return 0;
    }

    // Iterate entries, move all non-empty ones to new table's entries.
    for (size_t i = 0; i < table->capacity; i++) {
        ht_entry entry = table->entries[i];
        if (entry.key != NULL) {
            ht_set_entry(new_entries, new_capacity, entry.key,
                         entry.value, NULL);
        }
    }

    // Free old entries array and update this table's details.
    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
    return 1;
}

inline const char* ht_set(ht* table, const char* key, void* value) {
    assert(value != NULL);
    if (value == NULL) {
        return NULL;
    }

    // If length will exceed half of current capacity, expand it.
    if (table->length >= table->capacity / 2) {
        if (!ht_expand(table)) {
            return NULL;
        }
    }

    // Set entry and update length.
    return ht_set_entry(table->entries, table->capacity, key, value,
                        &table->length);
}

inline size_t ht_length(ht* table) {
    return table->length;
}

// Hash table iterator: create with ht_iterator, iterate with ht_next.
typedef struct {
    const char* key;  // current key
    void* value;      // current value

    // Don't use these fields directly.
    ht* _table;       // reference to hash table being iterated
    size_t _index;    // current index into ht._entries
} hti;

inline hti ht_iterator(ht* table) {
    hti it;
    it._table = table;
    it._index = 0;
    return it;
}

inline int ht_next(hti* it) {
    // Loop till we've hit end of entries array.
    ht* table = it->_table;
    while (it->_index < table->capacity) {
        size_t i = it->_index;
        it->_index++;
        if (table->entries[i].key != NULL) {
            // Found next non-empty item, update iterator key and value.
            ht_entry entry = table->entries[i];
            it->key = entry.key;
            it->value = entry.value;
            return 1;
        }
    }
    return 0;
}

inline void ht_exit_nomem(void) {
    fprintf(stderr, "hash_table: out of memory or other failure\n");
    exit(1);
}

inline void show_hash_table_int(ht *table, FILE *out) {
    assert (table);
    // Calculate average probe length.
    hti it = ht_iterator(table);
    size_t total_probes = 0;
    while (ht_next(&it)) {
        if (out==0) {
            printf("%s: %d\n", it.key, *(int*) it.value);
        }
        else {
            fprintf(out, "%s: %d\n", it.key, *(int*) it.value);
        }
    }
}

inline void show_hash_table_float(ht *table) {
    assert (table);
    hti it = ht_iterator(table);
    size_t total_probes = 0;
    while (ht_next(&it)) {
        printf("%s: %f\n", it.key, *(float *)(it.value));
    }
}

inline void hash_table_inc(ht *table, const char *key)
{

    void *value = ht_get(table, key);
    if (value != NULL) {
            // Already exists, increment int that value points to.
            int* pcount = (int*)value;
            (*pcount)++;
            return;
    }
    int* pcount = (int*)malloc(sizeof(int));
    //printf("hash_table_inc: created new entry for %s\n", key);

    if (pcount == NULL) {
        ht_exit_nomem();
    }
    *pcount = 1;
    if (ht_set(table, key, pcount) == NULL) {
        ht_exit_nomem();
    }
}


#endif