#ifndef REGINOLD_ST_H
#define REGINOLD_ST_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#if SIZEOF_LONG == SIZEOF_VOIDP
typedef unsigned long st_data_t;
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
typedef unsigned LONG_LONG st_data_t;
#else
# error ---->> st.c requires sizeof(void*) == sizeof(long) or sizeof(LONG_LONG) to be compiled. <<----
#endif

typedef st_data_t st_index_t;

#define MAX_ST_INDEX_VAL (~(st_index_t)0)
#define SIZEOF_ST_INDEX_T SIZEOF_VOIDP
#define ST_INDEX_BITS (SIZEOF_ST_INDEX_T * CHAR_BIT)

typedef struct st_table st_table;
typedef struct st_table_entry st_table_entry;

typedef int st_compare_func(st_data_t, st_data_t);
typedef st_index_t st_hash_func(st_data_t);

struct st_hash_type {
    int (*compare)(st_data_t, st_data_t);
    st_index_t (*hash)(st_data_t);
};

struct st_table {
    unsigned char entry_power, bin_power, size_ind;
    unsigned int rebuilds_num;
    const struct st_hash_type *type;
    st_index_t num_entries;
    st_index_t *bins;
    st_index_t entries_start, entries_bound;
    st_table_entry *entries;
};

enum st_retval {ST_CONTINUE, ST_STOP, ST_DELETE, ST_CHECK, ST_REPLACE};

typedef int st_update_callback_func(st_data_t *key, st_data_t *value, st_data_t arg, int existing);
typedef int st_foreach_callback_func(st_data_t, st_data_t, st_data_t);
typedef int st_foreach_check_callback_func(st_data_t, st_data_t, st_data_t, int);

size_t st_table_size(const struct st_table *tbl);
st_table *st_init_table(const struct st_hash_type *);
st_table *st_init_table_with_size(const struct st_hash_type *, st_index_t);
st_table *st_init_numtable(void);
st_table *st_init_numtable_with_size(st_index_t);
st_table *st_init_strtable(void);
st_table *st_init_strtable_with_size(st_index_t);
st_table *st_init_strcasetable(void);
st_table *st_init_strcasetable_with_size(st_index_t);
st_table *st_init_existing_table_with_size(st_table *tab, const struct st_hash_type *type, st_index_t size);
st_table *st_init_existing_numtable_with_size(st_table *tab, st_index_t size);
st_table *st_init_existing_strtable_with_size(st_table *tab, st_index_t size);
st_table *st_replace(st_table *new_tab, st_table *old_tab);
int st_delete(st_table *, st_data_t *, st_data_t *);
int st_delete_safe(st_table *, st_data_t *, st_data_t *, st_data_t);
int st_shift(st_table *, st_data_t *, st_data_t *);
int st_insert(st_table *, st_data_t, st_data_t);
int st_insert2(st_table *, st_data_t, st_data_t, st_data_t (*)(st_data_t));
int st_lookup(st_table *, st_data_t, st_data_t *);
int st_get_key(st_table *, st_data_t, st_data_t *);
int st_update(st_table *table, st_data_t key, st_update_callback_func *func, st_data_t arg);
int st_foreach_with_replace(st_table *tab, st_foreach_check_callback_func *func, st_update_callback_func *replace, st_data_t arg);
int st_foreach(st_table *, st_foreach_callback_func *, st_data_t);
int st_foreach_check(st_table *, st_foreach_check_callback_func *, st_data_t, st_data_t);
st_index_t st_keys(st_table *table, st_data_t *keys, st_index_t size);
st_index_t st_keys_check(st_table *table, st_data_t *keys, st_index_t size, st_data_t never);
st_index_t st_values(st_table *table, st_data_t *values, st_index_t size);
st_index_t st_values_check(st_table *table, st_data_t *values, st_index_t size, st_data_t never);
void st_add_direct(st_table *, st_data_t, st_data_t);
void st_free_table(st_table *);
void st_free_embedded_table(st_table *);
void st_cleanup_safe(st_table *, st_data_t);
void st_clear(st_table *);
st_table *st_copy(st_table *);
int st_numcmp(st_data_t, st_data_t);
st_index_t st_numhash(st_data_t);
int st_locale_insensitive_strcasecmp(const char *s1, const char *s2);
int st_locale_insensitive_strncasecmp(const char *s1, const char *s2, size_t n);
size_t st_memsize(const st_table *);
st_index_t st_hash(const void *ptr, size_t len, st_index_t h);
st_index_t st_hash_uint32(st_index_t h, uint32_t i);
st_index_t st_hash_uint(st_index_t h, st_index_t i);
st_index_t st_hash_end(st_index_t h);

#define st_is_member(table, key) st_lookup((table), (key), (st_data_t *)0)
#define st_hash_start(h) ((st_index_t)(h))

#ifndef RUBY_ASSERT
# define RUBY_ASSERT(expr) assert(expr)
#endif

#endif
