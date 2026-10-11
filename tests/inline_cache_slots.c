/* Unit test for diamond_cache_slot / diamond_cache_find (src/vm_internal.h),
 * the site-to-slot mapping shared by the method, extension and field caches.
 * Both are static inline, so this links nothing of the VM. */
#include "vm_internal.h"

#include <stdio.h>

static int checks = 0;

static void check(bool condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "inline_cache_slots: FAILED: %s\n", description);
        exit(1);
    }
    checks++;
}

/* Call-site tags are only compared, never dereferenced. */
static uint8_t code[1 << 16];

static size_t set_of(const uint8_t *site) {
    return diamond_cache_first_way(site) / DIAMOND_INLINE_CACHE_WAYS;
}

/* Collects `count` distinct sites that all hash to the same set. */
static void sites_in_one_set(const uint8_t **out, size_t count) {
    const size_t target = set_of(&code[0]);
    size_t found = 0;
    for (size_t index = 0; index < sizeof code && found < count; index++)
        if (set_of(&code[index]) == target) out[found++] = &code[index];
    check(found == count, "enough colliding sites in the test buffer");
}

static void claim(DiamondMethodCache *table, uint8_t *cursor, const uint8_t *site) {
    const size_t slot = diamond_cache_slot(site, table, sizeof table[0], cursor);
    if (table[slot].site != site) table[slot] = (DiamondMethodCache){.site = site};
}

int main(void) {
    static DiamondMethodCache table[DIAMOND_INLINE_CACHE_COUNT];
    uint8_t cursor = 0;
    const uint8_t *sites[DIAMOND_INLINE_CACHE_WAYS + 2];
    sites_in_one_set(sites, DIAMOND_INLINE_CACHE_WAYS + 2);

    check(DIAMOND_INLINE_CACHE_COUNT % DIAMOND_INLINE_CACHE_WAYS == 0,
          "capacity is a whole number of sets");
    check(diamond_cache_find(sites[0], table, sizeof table[0]) == SIZE_MAX,
          "an unseen site is not found");

    /* As many colliding sites as there are ways all stay resident. */
    for (size_t index = 0; index < DIAMOND_INLINE_CACHE_WAYS; index++)
        claim(table, &cursor, sites[index]);
    for (size_t index = 0; index < DIAMOND_INLINE_CACHE_WAYS; index++) {
        const size_t slot = diamond_cache_find(sites[index], table, sizeof table[0]);
        check(slot != SIZE_MAX, "a colliding site stays resident");
        check(table[slot].site == sites[index], "found slot carries the site tag");
        check(slot / DIAMOND_INLINE_CACHE_WAYS == set_of(sites[index]),
              "slot lies inside the site's set");
        check(diamond_cache_slot(sites[index], table, sizeof table[0], &cursor) == slot,
              "claiming a resident site returns its own slot");
    }
    check(cursor == 0, "no victim was chosen while a way was free");

    /* One more evicts exactly one resident, and only that set is touched. */
    claim(table, &cursor, sites[DIAMOND_INLINE_CACHE_WAYS]);
    size_t resident = 0;
    for (size_t index = 0; index <= DIAMOND_INLINE_CACHE_WAYS; index++)
        if (diamond_cache_find(sites[index], table, sizeof table[0]) != SIZE_MAX) resident++;
    check(resident == DIAMOND_INLINE_CACHE_WAYS, "overflow evicts exactly one site");
    check(cursor == 1, "the victim cursor advanced once");

    /* A site in a different set is unaffected by that churn. */
    const uint8_t *other = nullptr;
    for (size_t index = 0; index < sizeof code; index++)
        if (set_of(&code[index]) != set_of(sites[0])) { other = &code[index]; break; }
    check(other != nullptr, "found a site in another set");
    claim(table, &cursor, other);
    for (size_t round = 0; round < 3 * DIAMOND_INLINE_CACHE_WAYS; round++)
        claim(table, &cursor, sites[round % (DIAMOND_INLINE_CACHE_WAYS + 2)]);
    check(diamond_cache_find(other, table, sizeof table[0]) != SIZE_MAX,
          "churn in one set leaves other sets alone");

    printf("inline_cache_slots: %d checks passed\n", checks);
    return 0;
}
