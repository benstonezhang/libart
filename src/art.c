#include <stdlib.h>
#include <string.h>
#include "art.h"

#define MAX_PREFIX_LEN 10

#define NODE4   1
#define NODE16  2
#define NODE48  3
#define NODE256 4

/**
 * Macros to manipulate pointer tags
 */
#define IS_LEAF(x) (((uintptr_t)x & 1))
#define SET_LEAF(x) ((void*)((uintptr_t)x | 1))
#define LEAF_RAW(x) ((art_leaf*)((void*)((uintptr_t)x >> 1 << 1)))

/**
 * This struct is included as part of all the various node sizes
 */
typedef struct {
    uint32_t partial_len;
    uint8_t type;
    uint8_t num_children;
    unsigned char partial[MAX_PREFIX_LEN];
} art_node;

/**
 * Small node with only 4 children
 */
typedef struct {
    art_node n;
    unsigned char keys[4];
    art_node *children[4];
    art_leaf *me;
} art_node4;

/**
 * Node with 16 children
 */
typedef struct {
    art_node n;
    unsigned char keys[16];
    art_node *children[16];
    art_leaf *me;
} art_node16;

/**
 * Node with 48 children, but
 * a full 256 byte field.
 */
typedef struct {
    art_node n;
    unsigned char keys[256];
    art_node *children[48];
    art_leaf *me;
} art_node48;

/**
 * Full node with 256 children
 */
typedef struct {
    art_node n;
    art_node *children[256];
    art_leaf *me;
} art_node256;

#ifdef __SSE2__
#include <emmintrin.h>
#elif defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/* From public domain
 * https://graphics.stanford.edu/~seander/bithacks.html
 *
 * Bit twiddling:
 * contains_byte:  __builtin_ctz:  for key index:
 *    0x80000000            0x20               3
 *      0x800000            0x18               2
 *      0x808000            0x10               1
 *          0x80             0x8               0
 *           0x0             0x0       not found
 */
static inline uint32_t word_has_byte(uint32_t v, unsigned char c, uint32_t num) {
    v = (v ^ (0x01010101UL * c)) | ((~0ULL) << (num << 3));
    return (v - 0x01010101UL) & ~v & 0x80808080UL;
}

/**
 * Allocates a node of the given type,
 * initializes to zero and sets the type.
 */
static art_node* alloc_node(uint8_t type) {
    art_node* n;
    size_t size;
    switch (type) {
        case NODE4:
            size = sizeof(art_node4);
            break;
        case NODE16:
            size = sizeof(art_node16);
            break;
        case NODE48:
            size = sizeof(art_node48);
            break;
        case NODE256:
            size = sizeof(art_node256);
            break;
        default:
            abort();
    }
    n = (art_node*)calloc(1, size);
    n->type = type;
    return n;
}

static art_leaf* node_get_own_leaf(const art_node* n) {
    switch (n->type) {
        case NODE4:
            return ((art_node4*)n)->me;
        case NODE16:
            return ((art_node16*)n)->me;
        case NODE48:
            return ((art_node48*)n)->me;
        case NODE256:
            return ((art_node256*)n)->me;
        default:
            abort();
    }
}

static art_leaf** node_get_own_leaf_ptr(const art_node* n) {
    switch (n->type) {
        case NODE4:
            return &((art_node4*)n)->me;
        case NODE16:
            return &((art_node16*)n)->me;
        case NODE48:
            return &((art_node48*)n)->me;
        case NODE256:
            return &((art_node256*)n)->me;
        default:
            abort();
    }
}

static void node_set_own_leaf(art_node* n, art_leaf* l) {
    switch (n->type) {
        case NODE4:
            ((art_node4*)n)->me = l;
            break;
        case NODE16:
            ((art_node16*)n)->me = l;
            break;
        case NODE48:
            ((art_node48*)n)->me = l;
            break;
        case NODE256:
            ((art_node256*)n)->me = l;
            break;
        default:
            abort();
    }
}

/**
 * Initializes an ART tree
 * @return 0 on success.
 */
int art_tree_init(art_tree *t) {
    t->root = NULL;
    t->size = 0;
    return 0;
}

// Recursively destroys the tree
static void destroy_node(art_node *n) {
    // Break if null
    if (!n) return;

    // Special case leafs
    if (IS_LEAF(n)) {
        free(LEAF_RAW(n));
        return;
    }

    // Handle each node type
    int i;

    switch (n->type) {
        case NODE4:
            for (i=0;i<n->num_children;i++)
                destroy_node(((art_node4*)n)->children[i]);
            break;

        case NODE16:
            for (i=0;i<n->num_children;i++)
                destroy_node(((art_node16*)n)->children[i]);
            break;

        case NODE48:
            for (i=0;i<256;i++) {
                int idx = ((art_node48*)n)->keys[i];
                if (!idx) continue;
                destroy_node(((art_node48*)n)->children[idx-1]);
            }
            break;

        case NODE256:
            for (i=0;i<256;i++)
                if (((art_node256*)n)->children[i])
                    destroy_node(((art_node256*)n)->children[i]);
            break;

        default:
            abort();
    }

    art_leaf* l = node_get_own_leaf(n);
    if (l)
        free(l);

    // Free ourself on the way up
    free(n);
}

/**
 * Destroys an ART tree
 * @return 0 on success.
 */
int art_tree_destroy(art_tree *t) {
    destroy_node(t->root);
    return 0;
}

/**
 * Returns the size of the ART tree.
 */

#ifndef BROKEN_GCC_C99_INLINE
extern inline uint64_t art_size(art_tree *t);
#endif

static art_node** find_child(art_node *n, unsigned char c) {
    switch (n->type) {
        case NODE4:
        {
            art_node4 *p = (art_node4*)n;

#if defined(__SSE2__) && defined(FORCE_SSE2)
            // Compare the key to all 4 stored keys
            __m128i cmp = _mm_cmpeq_epi8(_mm_set1_epi8(c),
                    _mm_cvtsi32_si128(*(int *)p->keys));

            // Use a mask to ignore children that don't exist
            uint32_t mask = (1UL << n->num_children) - 1;
            uint32_t bitfield = _mm_movemask_epi8(cmp) & mask;

            // If we have a match (any bit set) then we can return
            // the pointer match using ctz to get the index.
            if (bitfield)
                return &p->children[__builtin_ctz(bitfield)];
#elif defined(__ARM_NEON__) && defined(FORCE_ARM_NEON)
            // Compare the key to all 4 stored keys
            uint8x16_t cmp = vceqq_u8(vdupq_n_u8(c),
                    vreinterpretq_u8_u32( vsetq_lane_u32(
                            *(uint32_t *)p->keys, vdupq_n_u32(0), 0)));

            // Use a mask to ignore children that don't exist
            uint64_t mask = (((uint64_t)1) << (n->num_children << 3)) - 1;
            uint64_t bitfield = vget_lane_u64(vget_low_u64(
                    vreinterpretq_u64_u8(cmp)), 0) & mask;

            // If we have a match (any bit set) then we can return
            // the pointer match using ctz to get the index.
            if (bitfield)
#if LONG_WIDTH >= 8
                return &p->children[__builtin_ctzl(bitfield) >> 3];
#else
                return &p->children[__builtin_ctzll(bitfield) >> 3];
#endif
#elif __INT_WIDTH__ >= 4
            uint32_t bitfield = word_has_byte(*(uint32_t*)p->keys, c, n->num_children);
            if (bitfield)
                return &p->children[__builtin_ctz(bitfield) >> 3];
#else
            for (int i=0 ; i < n->num_children; i++) {
                // this cast works around a bug in gcc 5.1 when unrolling loops
                // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=59124
                if (((unsigned char*)p->keys)[i] == c)
                    return &p->children[i];
            }
#endif
            break;
        }

        case NODE16:
        {
            art_node16 *p = (art_node16*)n;

#ifdef __SSE2__
            // Compare the key to all 16 stored keys
            __m128i cmp = _mm_cmpeq_epi8(_mm_set1_epi8(c),
                    _mm_loadu_si128((__m128i*)p->keys));

            // Use a mask to ignore children that don't exist
            uint32_t mask = (1UL << n->num_children) - 1;
            uint32_t bitfield = _mm_movemask_epi8(cmp) & mask;

            // If we have a match (any bit set) then we can return
            // the pointer match using ctz to get the index.
            if (bitfield)
                return &p->children[__builtin_ctz(bitfield)];
#elif defined(__ARM_NEON__)
            // Compare the key to all 16 stored keys
            uint8x8_t cmp = vshrn_n_u16(vreinterpretq_u16_u8(
                    vceqq_u8(vdupq_n_u8(c), vld1q_u8(p->keys))), 4);

            // Use a mask to ignore children that don't exist
            uint64_t mask = (n->num_children == 16) ?
                    ~0ULL : (1ULL << (n->num_children << 2)) - 1;
            uint64_t bitfield = vget_lane_u64(vreinterpret_u64_u8(cmp), 0) & mask;

            // If we have a match (any bit set) then we can return
            // the pointer match using ctz to get the index.
            if (bitfield)
#if LONG_WIDTH >= 8
                return &p->children[__builtin_ctzl(bitfield) >> 2];
#else
                return &p->children[__builtin_ctzll(bitfield) >> 2];
#endif
#else
            // Compare the key to all 16 stored keys
            for (int i = 0; i < n->num_children; i++)
                if (p->keys[i] == c)
                    return &p->children[i];
#endif
            break;
        }

        case NODE48:
        {
            art_node48 *p = (art_node48*)n;
            int i = p->keys[c];
            if (i)
                return &p->children[i-1];
            break;
        }

        case NODE256:
        {
            art_node256 *p = (art_node256*)n;
            if (p->children[c])
                return &p->children[c];
            break;
        }

        default:
            abort();
    }
    return NULL;
}

// Simple inlined if
static inline int min(int a, int b) {
    return (a < b) ? a : b;
}

/**
 * Returns the number of prefix characters shared between
 * the key and node.
 */
static int check_prefix(const art_node *n, const unsigned char *key, int key_len, int depth) {
    int max_cmp = min(min(n->partial_len, MAX_PREFIX_LEN), key_len - depth);
    int idx;
    for (idx=0; idx < max_cmp; idx++) {
        if (n->partial[idx] != key[depth+idx])
            return idx;
    }
    return idx;
}

/**
 * Checks if a leaf matches
 * @return 0 on success.
 */
static int leaf_matches(const art_leaf *n, const unsigned char *key, int key_len, int depth) {
    (void)depth;
    // Fail if the key lengths are different
    if (n->key_len != (uint32_t)key_len) return 1;

    // Compare the keys starting at the depth
    return memcmp(n->key, key, key_len);
}

/**
 * Searches for a value in the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_search(const art_tree *t, const unsigned char *key, int key_len) {
    art_node **child;
    art_node *n = t->root;
    int prefix_len, depth = 0;
    while (n) {
        // Might be a leaf
        if (IS_LEAF(n)) {
            n = (art_node*)LEAF_RAW(n);
            // Check if the expanded path matches
            if (!leaf_matches((art_leaf*)n, key, key_len, depth)) {
                return ((art_leaf*)n)->value;
            }
            return NULL;
        }

        // Bail if the prefix does not match
        if (n->partial_len) {
            prefix_len = check_prefix(n, key, key_len, depth);
            if (prefix_len != min(MAX_PREFIX_LEN, n->partial_len))
                return NULL;
            depth += n->partial_len;
        }

        // Might on itself
        if (depth == key_len) {
            art_leaf* l = node_get_own_leaf(n);
            return l ? l->value : NULL;
        }

        // Recursively search
        child = find_child(n, key[depth]);
        n = (child) ? *child : NULL;
        depth++;
    }
    return NULL;
}

// Find the minimum leaf under a node
static art_leaf* minimum(const art_node *n) {
    // Handle base cases
    if (!n) return NULL;
    if (IS_LEAF(n)) return LEAF_RAW(n);

    art_leaf* l = node_get_own_leaf(n);
    if (l) return l;

    int idx;
    switch (n->type) {
        case NODE4:
            return minimum(((const art_node4*)n)->children[0]);
        case NODE16:
            return minimum(((const art_node16*)n)->children[0]);
        case NODE48:
            idx=0;
            while (!((const art_node48*)n)->keys[idx]) idx++;
            idx = ((const art_node48*)n)->keys[idx] - 1;
            return minimum(((const art_node48*)n)->children[idx]);
        case NODE256:
            idx=0;
            while (!((const art_node256*)n)->children[idx]) idx++;
            return minimum(((const art_node256*)n)->children[idx]);
        default:
            abort();
    }
}

// Find the maximum leaf under a node
static art_leaf* maximum(const art_node *n) {
    // Handle base cases
    if (!n) return NULL;
    if (IS_LEAF(n)) return LEAF_RAW(n);

    int idx;
    switch (n->type) {
        case NODE4:
            return maximum(((const art_node4*)n)->children[n->num_children-1]);
        case NODE16:
            return maximum(((const art_node16*)n)->children[n->num_children-1]);
        case NODE48:
            idx=255;
            while (!((const art_node48*)n)->keys[idx]) idx--;
            idx = ((const art_node48*)n)->keys[idx] - 1;
            return maximum(((const art_node48*)n)->children[idx]);
        case NODE256:
            idx=255;
            while (!((const art_node256*)n)->children[idx]) idx--;
            return maximum(((const art_node256*)n)->children[idx]);
        default:
            abort();
    }
}

/**
 * Returns the minimum valued leaf
 */
art_leaf* art_minimum(art_tree *t) {
    return minimum((art_node*)t->root);
}

/**
 * Returns the maximum valued leaf
 */
art_leaf* art_maximum(art_tree *t) {
    return maximum((art_node*)t->root);
}

static art_leaf* make_leaf(const unsigned char *key, int key_len, void *value) {
    art_leaf *l = (art_leaf*)calloc(1, sizeof(art_leaf)+key_len);
    l->value = value;
    l->key_len = key_len;
    memcpy(l->key, key, key_len);
    return l;
}

static int longest_common_prefix(art_leaf *l1, art_leaf *l2, int depth) {
    int max_cmp = min(l1->key_len, l2->key_len) - depth;
    int idx;
    for (idx=0; idx < max_cmp; idx++) {
        if (l1->key[depth+idx] != l2->key[depth+idx])
            return idx;
    }
    return idx;
}

static void copy_header(art_node *dest, art_node *src) {
    dest->num_children = src->num_children;
    dest->partial_len = src->partial_len;
    memcpy(dest->partial, src->partial, min(MAX_PREFIX_LEN, src->partial_len));
}

static void add_child256(art_node256 *n, art_node **ref, unsigned char c, void *child) {
    (void)ref;
    n->n.num_children++;
    n->children[c] = (art_node*)child;
}

static void add_child48(art_node48 *n, art_node **ref, unsigned char c, void *child) {
    if (n->n.num_children < 48) {
        int pos = 0;
        while (n->children[pos]) pos++;
        n->children[pos] = (art_node*)child;
        n->keys[c] = pos + 1;
        n->n.num_children++;
    } else {
        art_node256 *new_node = (art_node256*)alloc_node(NODE256);
        for (int i=0;i<256;i++) {
            if (n->keys[i]) {
                new_node->children[i] = n->children[n->keys[i] - 1];
            }
        }
        copy_header((art_node*)new_node, (art_node*)n);
        new_node->me = n->me;
        *ref = (art_node*)new_node;
        free(n);
        add_child256(new_node, ref, c, child);
    }
}

static void add_child16(art_node16 *n, art_node **ref, unsigned char c, void *child) {
    if (n->n.num_children < 16) {
        int idx;

#ifdef __SSE2__
        // Compare the key to all 16 stored keys
        __m128i cmp = _mm_cmplt_epi8(_mm_set1_epi8(c),
                _mm_loadu_si128((__m128i*)n->keys));

        // Use a mask to ignore children that don't exist
        uint32_t mask = (1UL << n->n.num_children) - 1;
        uint32_t bitfield = _mm_movemask_epi8(cmp) & mask;

        // Check if less than any
        if (bitfield) {
            idx = __builtin_ctz(bitfield);
            memmove(n->keys+idx+1,n->keys+idx,n->n.num_children-idx);
            memmove(n->children+idx+1,n->children+idx,
                    (n->n.num_children-idx)*sizeof(void*));
        } else
            idx = n->n.num_children;
#elif defined(__ARM_NEON__)
        // Compare the key to all 16 stored keys
        uint8x8_t cmp = vshrn_n_u16(vreinterpretq_u16_u8(
                vcltq_u8(vdupq_n_u8(c), vld1q_u8(n->keys))), 4);

        // Use a mask to ignore children that don't exist
        uint64_t mask = (n->n.num_children == 16) ?
                ~0ULL : (1ULL << (n->n.num_children << 2)) - 1;
        uint64_t bitfield = vget_lane_u64(vreinterpret_u64_u8(cmp), 0) & mask;

        // Check if less than any
        if (bitfield) {
#if __LONG_WIDTH__ >= 8
            idx = __builtin_ctzl(bitfield) >> 2;
#else
            idx = __builtin_ctzll(bitfield) >> 2;
#endif
            memmove(n->keys+idx+1,n->keys+idx,n->n.num_children-idx);
            memmove(n->children+idx+1,n->children+idx,
                    (n->n.num_children-idx)*sizeof(void*));
        } else
            idx = n->n.num_children;
#else
        // Compare the key to all stored keys
        for (idx = 0; idx < n->n.num_children; idx++) {
            if (c < n->keys[idx]) {
                memmove(n->keys+idx+1, n->keys+idx, n->n.num_children-idx);
                memmove(n->children+idx+1, n->children+idx,
                        (n->n.num_children-idx)*sizeof(void*));
                break;
            }
        }
#endif

        // Set the child
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;

    } else {
        art_node48 *new_node = (art_node48*)alloc_node(NODE48);

        // Copy the child pointers and populate the key map
        memcpy(new_node->children, n->children, 16*sizeof(void*));
        for (int i=0;i<16;i++)
            new_node->keys[n->keys[i]] = i + 1;
        copy_header((art_node*)new_node, (art_node*)n);
        new_node->me = n->me;
        *ref = (art_node*)new_node;
        free(n);
        add_child48(new_node, ref, c, child);
    }
}

static void add_child4(art_node4 *n, art_node **ref, unsigned char c, void *child) {
    if (n->n.num_children < 4) {
        int idx;

#if defined(__SSE2__) && defined(FORCE_SSE2)
        // Compare the key to all 4 stored keys
        __m128i cmp = _mm_cmplt_epi8(_mm_set1_epi8(c),
                _mm_cvtsi32_si128(*(int *)n->keys));

        // Use a mask to ignore children that don't exist
        uint32_t mask = (1 << n->n.num_children) - 1;
        uint32_t bitfield = _mm_movemask_epi8(cmp) & mask;

        // If we have a match (any bit set) then we can return
        // the pointer match using ctz to get the index.
        if (bitfield) {
            idx = __builtin_ctz(bitfield);
            // Shift to make room
            memmove(n->keys+idx+1, n->keys+idx, n->n.num_children-idx);
            memmove(n->children+idx+1, n->children+idx,
                    (n->n.num_children-idx)*sizeof(void*));
        } else
            idx = n->n.num_children;
#elif defined(__ARM_NEON__) && defined(FORCE_ARM_NEON)
        uint8x16_t cmp = vcltq_u8(vdupq_n_u8(c),
                vreinterpretq_u8_u32( vsetq_lane_u32(
                        *(uint32_t *)n->keys, vdupq_n_u32(0), 0)));

        uint64_t mask = (((uint64_t)1) << (n->n.num_children << 3)) - 1;
        uint64_t bitfield = vget_lane_u64(vget_low_u64(
                vreinterpretq_u64_u8(cmp)), 0) & mask;

        if (bitfield) {
#if __LONG_WIDTH__ >= 8
            idx = __builtin_ctzl(bitfield) >> 3;
#else
            idx = __builtin_ctzll(bitfield) >> 3;
#endif
            // Shift to make room
            memmove(n->keys+idx+1, n->keys+idx, n->n.num_children-idx);
            memmove(n->children+idx+1, n->children+idx,
                    (n->n.num_children-idx)*sizeof(void*));
        } else
            idx = n->n.num_children;
#elif __INT_WIDTH__ >= 4
        uint64_t cmp = ((0x0001000100010001ULL * c) | 0x1000100010001000ULL) -
                (((uint64_t)n->keys[0]) | ((uint64_t)n->keys[1]) << 16 |
                 ((uint64_t)n->keys[2]) << 32 | ((uint64_t)n->keys[3]) << 48);
        uint64_t mask = (1ULL << (n->n.num_children << 4)) - 1;
        uint64_t bitfield = ((cmp & 0x1000100010001000ULL) ^ 0x1000100010001000ULL) & mask;
        if (bitfield) {
#if __LONG_WIDTH__ >= 8
            idx = __builtin_ctzl(bitfield) >> 4;
#else
            idx = __builtin_ctzll(bitfield) >> 4;
#endif
            // Shift to make room
            memmove(n->keys+idx+1, n->keys+idx, n->n.num_children-idx);
            memmove(n->children+idx+1, n->children+idx,
                    (n->n.num_children-idx)*sizeof(void*));
        } else
            idx = n->n.num_children;
#else
        for (idx=0; idx < n->n.num_children; idx++) {
            if (c < n->keys[idx]) {
                memmove(n->keys+idx+1, n->keys+idx, n->n.num_children-idx);
                memmove(n->children+idx+1, n->children+idx,
                        (n->n.num_children-idx)*sizeof(void*));
                break;
            }
        }
#endif

        // Insert element
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;
    } else {
        art_node16 *new_node = (art_node16*)alloc_node(NODE16);

        // Copy the child pointers and the key map
        memcpy(new_node->children, n->children, 4*sizeof(void*));
        memcpy(new_node->keys, n->keys, 4*sizeof(unsigned char));
        copy_header((art_node*)new_node, (art_node*)n);
        new_node->me = n->me;
        *ref = (art_node*)new_node;
        free(n);
        add_child16(new_node, ref, c, child);
    }
}

static void add_child(art_node *n, art_node **ref, unsigned char c, void *child) {
    switch (n->type) {
        case NODE4:
            return add_child4((art_node4*)n, ref, c, child);
        case NODE16:
            return add_child16((art_node16*)n, ref, c, child);
        case NODE48:
            return add_child48((art_node48*)n, ref, c, child);
        case NODE256:
            return add_child256((art_node256*)n, ref, c, child);
        default:
            abort();
    }
}

/**
 * Calculates the index at which the prefixes mismatch
 */
static int prefix_mismatch(const art_node *n, const unsigned char *key, int key_len, int depth) {
    int max_cmp = min(min(MAX_PREFIX_LEN, n->partial_len), key_len - depth);
    int idx;
    for (idx=0; idx < max_cmp; idx++) {
        if (n->partial[idx] != key[depth+idx])
            return idx;
    }

    // If the prefix is short we can avoid finding a leaf
    if (n->partial_len > MAX_PREFIX_LEN) {
        // Prefix is longer than what we've checked, find a leaf
        art_leaf *l = minimum(n);
        max_cmp = min(l->key_len, key_len)- depth;
        for (; idx < max_cmp; idx++) {
            if (l->key[idx+depth] != key[depth+idx])
                return idx;
        }
    }
    return idx;
}

static void* recursive_insert(art_node *n, art_node **ref, const unsigned char *key,
        int key_len, void *value, int depth, int *old, int replace) {
    // If we are at a NULL node, inject a leaf
    if (!n) {
        *ref = (art_node*)SET_LEAF(make_leaf(key, key_len, value));
        return NULL;
    }

    // If we are at a leaf, we need to replace it with a node
    if (IS_LEAF(n)) {
        art_leaf *l = LEAF_RAW(n);

        // Check if we are updating an existing value
        if (!leaf_matches(l, key, key_len, depth)) {
            *old = 1;
            void *old_val = l->value;
            if(replace) l->value = value;
            return old_val;
        }

        // New value, we must split the leaf into a node4
        art_node4 *new_node = (art_node4*)alloc_node(NODE4);

        // Create a new leaf
        art_leaf *l2 = make_leaf(key, key_len, value);

        // Determine longest prefix
        int longest_prefix = longest_common_prefix(l, l2, depth);
        new_node->n.partial_len = longest_prefix;
        if (longest_prefix) {
            memcpy(new_node->n.partial, key+depth, min(MAX_PREFIX_LEN, longest_prefix));
            // Add the leafs to the new node4
            add_child4(new_node, ref, l->key[depth+longest_prefix], SET_LEAF(l));
            add_child4(new_node, ref, l2->key[depth+longest_prefix], SET_LEAF(l2));
        } else {
            if (l->key_len == depth) {
                node_set_own_leaf(&new_node->n, l);
                add_child4(new_node, ref, l2->key[depth+longest_prefix], SET_LEAF(l2));
            } else if (l2->key_len == depth) {
                add_child4(new_node, ref, l->key[depth+longest_prefix], SET_LEAF(l));
                node_set_own_leaf(&new_node->n, l2);
            } else {
                add_child4(new_node, ref, l->key[depth+longest_prefix], SET_LEAF(l));
                add_child4(new_node, ref, l2->key[depth+longest_prefix], SET_LEAF(l2));
            }
        }
        *ref = (art_node*)new_node;
        return NULL;
    }

    // Check if given node has a prefix
    if (n->partial_len) {
        // Determine if the prefixes differ, since we need to split
        int prefix_diff = prefix_mismatch(n, key, key_len, depth);
        if ((uint32_t)prefix_diff >= n->partial_len) {
            depth += n->partial_len;
            goto recurse_search;
        }

        // Create a new node
        art_node4 *new_node = (art_node4*)alloc_node(NODE4);
        *ref = (art_node*)new_node;
        new_node->n.partial_len = prefix_diff;
        memcpy(new_node->n.partial, n->partial, min(MAX_PREFIX_LEN, prefix_diff));

        // Adjust the prefix of the old node
        if (n->partial_len <= MAX_PREFIX_LEN) {
            add_child4(new_node, ref, n->partial[prefix_diff], n);
            n->partial_len -= (prefix_diff+1);
            memmove(n->partial, n->partial+prefix_diff+1,
                    min(MAX_PREFIX_LEN, n->partial_len));
        } else {
            n->partial_len -= (prefix_diff+1);
            art_leaf *l = minimum(n);
            add_child4(new_node, ref, l->key[depth+prefix_diff], n);
            memcpy(n->partial, l->key+depth+prefix_diff+1,
                    min(MAX_PREFIX_LEN, n->partial_len));
        }

        // Insert the new leaf
        art_leaf* l = make_leaf(key, key_len, value);
        if (depth+prefix_diff < key_len)
            add_child4(new_node, ref, key[depth+prefix_diff], SET_LEAF(l));
        else
            node_set_own_leaf(&new_node->n, l);
        return NULL;
    }

    if (depth == key_len) {
update_node_own_leaf:;
        art_leaf* l = node_get_own_leaf(n);
        if (l) {
            *old = 1;
            void* old_val = l->value;
            if (replace) l->value = value;
            return old_val;
        } else {
            l = make_leaf(key, key_len, value);
            node_set_own_leaf(n, l);
            return NULL;
        }
    }

recurse_search:;

    // Find a child to recurse to
    art_node **child = find_child(n, key[depth]);
    if (child) {
        return recursive_insert(*child, child, key, key_len, value, depth+1, old, replace);
    }

    // No child, node goes within us
    art_leaf *l = make_leaf(key, key_len, value);
    if (depth < key_len)
        add_child(n, ref, key[depth], SET_LEAF(l));
    else
        goto update_node_own_leaf;

    return NULL;
}

/**
 * inserts a new value into the art tree
 * @arg t the tree
 * @arg key the key
 * @arg key_len the length of the key
 * @arg value opaque value.
 * @return null if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_insert(art_tree *t, const unsigned char *key, int key_len, void *value) {
    int old_val = 0;
    void *old = recursive_insert(t->root, (art_node**)&t->root, key, key_len, value, 0, &old_val, 1);
    if (!old_val) t->size++;
    return old;
}

/**
 * inserts a new value into the art tree (no replace)
 * @arg t the tree
 * @arg key the key
 * @arg key_len the length of the key
 * @arg value opaque value.
 * @return null if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_insert_no_replace(art_tree *t, const unsigned char *key, int key_len, void *value) {
    int old_val = 0;
    void *old = recursive_insert(t->root, (art_node**)&t->root, key, key_len, value, 0, &old_val, 0);
    if (!old_val) t->size++;
    return old;
}

static void remove_child256(art_node256 *n, art_node **ref, unsigned char c) {
    n->children[c] = NULL;
    n->n.num_children--;

    // Resize to a node48 on underflow, not immediately to prevent
    // trashing if we sit on the 48/49 boundary
    if (n->n.num_children == 37) {
        art_node48 *new_node = (art_node48*)alloc_node(NODE48);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        int pos = 0;
        for (int i=0;i<256;i++) {
            if (n->children[i]) {
                new_node->children[pos] = n->children[i];
                new_node->keys[i] = pos + 1;
                pos++;
            }
        }
        new_node->me = n->me;
        free(n);
    }
}

static void remove_child48(art_node48 *n, art_node **ref, unsigned char c) {
    int pos = n->keys[c];
    n->keys[c] = 0;
    n->children[pos-1] = NULL;
    n->n.num_children--;

    if (n->n.num_children == 12) {
        art_node16 *new_node = (art_node16*)alloc_node(NODE16);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        int child = 0;
        for (int i=0;i<256;i++) {
            pos = n->keys[i];
            if (pos) {
                new_node->keys[child] = i;
                new_node->children[child] = n->children[pos - 1];
                child++;
            }
        }
        new_node->me = n->me;
        free(n);
    }
}

static void remove_child16(art_node16 *n, art_node **ref, art_node **l) {
    int pos = l - n->children;
    memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
    memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
    n->n.num_children--;

    if (n->n.num_children == 3) {
        art_node4 *new_node = (art_node4*)alloc_node(NODE4);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);
        memcpy(new_node->keys, n->keys, 4);
        memcpy(new_node->children, n->children, 4*sizeof(void*));
        new_node->me = n->me;
        free(n);
    }
}

static void remove_child4(art_node4 *n, art_node **ref, art_node **l) {
    int pos = l - n->children;
    art_leaf* nl;
    if (pos >= 0 && pos < n->n.num_children) {
        memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
        memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
        n->n.num_children--;
        nl = NULL;
    } else {
        art_leaf** lp = node_get_own_leaf_ptr(&n->n);
        nl = *lp;
        if ((art_node**)lp == l) {
            if (n->n.num_children == 0) {
                *ref = (art_node*)nl;
                free(n);
                return;
            }
        }
        node_set_own_leaf(&n->n, NULL);
    }

    // Remove nodes with only a single child
    if (nl == NULL && n->n.num_children == 1) {
        art_node *child = n->children[0];
        if (!IS_LEAF(child)) {
            // Concatenate the prefixes
            int prefix = n->n.partial_len;
            if (prefix < MAX_PREFIX_LEN) {
                n->n.partial[prefix] = n->keys[0];
                prefix++;
            }
            if (prefix < MAX_PREFIX_LEN) {
                int sub_prefix = min(child->partial_len, MAX_PREFIX_LEN - prefix);
                memcpy(n->n.partial+prefix, child->partial, sub_prefix);
                prefix += sub_prefix;
            }

            // Store the prefix in the child
            memcpy(child->partial, n->n.partial, min(prefix, MAX_PREFIX_LEN));
            child->partial_len += n->n.partial_len + 1;
        }
        *ref = child;
        free(n);
    }
}

static void remove_child(art_node *n, art_node **ref, unsigned char c, art_node **l) {
    switch (n->type) {
        case NODE4:
            return remove_child4((art_node4*)n, ref, l);
        case NODE16:
            return remove_child16((art_node16*)n, ref, l);
        case NODE48:
            return remove_child48((art_node48*)n, ref, c);
        case NODE256:
            return remove_child256((art_node256*)n, ref, c);
        default:
            abort();
    }
}

static art_leaf* recursive_delete(art_node *n, art_node **ref, const unsigned char *key, int key_len, int depth) {
    // Search terminated
    if (!n) return NULL;

    // Handle hitting a leaf node
    if (IS_LEAF(n)) {
        art_leaf *l = LEAF_RAW(n);
        if (!leaf_matches(l, key, key_len, depth)) {
            *ref = NULL;
            return l;
        }
        return NULL;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
        int prefix_len = check_prefix(n, key, key_len, depth);
        if (prefix_len != min(MAX_PREFIX_LEN, n->partial_len)) {
            return NULL;
        }
        depth = depth + n->partial_len;
    }

    if (depth == key_len) {
        art_leaf* l = node_get_own_leaf(n);
        if (l)
            node_set_own_leaf(n, NULL);
        return l;
    }

    // Find child node
    art_node **child = find_child(n, key[depth]);
    if (!child) return NULL;

    // If the child is leaf, delete from this node
    if (IS_LEAF(*child)) {
        art_leaf *l = LEAF_RAW(*child);
        if (!leaf_matches(l, key, key_len, depth)) {
            remove_child(n, ref, key[depth], child);
            return l;
        }
        return NULL;

    // Recurse
    } else {
        return recursive_delete(*child, child, key, key_len, depth+1);
    }
}

/**
 * Deletes a value from the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_delete(art_tree *t, const unsigned char *key, int key_len) {
    art_leaf *l = recursive_delete(t->root, (art_node**)&t->root, key, key_len, 0);
    if (l) {
        t->size--;
        void *old = l->value;
        free(l);
        return old;
    }
    return NULL;
}

// Recursively iterates over the tree
static int recursive_iter(art_node *n, art_callback cb, void *data) {
    // Handle base cases
    if (!n) return 0;
    if (IS_LEAF(n)) {
        art_leaf *l = LEAF_RAW(n);
        return cb(data, (const unsigned char*)l->key, l->key_len, l->value);
    }

    int idx, res;
    switch (n->type) {
        case NODE4:
            for (int i=0; i < n->num_children; i++) {
                res = recursive_iter(((art_node4*)n)->children[i], cb, data);
                if (res) return res;
            }
            break;

        case NODE16:
            for (int i=0; i < n->num_children; i++) {
                res = recursive_iter(((art_node16*)n)->children[i], cb, data);
                if (res) return res;
            }
            break;

        case NODE48:
            for (int i=0; i < 256; i++) {
                idx = ((art_node48*)n)->keys[i];
                if (!idx) continue;

                res = recursive_iter(((art_node48*)n)->children[idx-1], cb, data);
                if (res) return res;
            }
            break;

        case NODE256:
            for (int i=0; i < 256; i++) {
                if (!((art_node256*)n)->children[i]) continue;
                res = recursive_iter(((art_node256*)n)->children[i], cb, data);
                if (res) return res;
            }
            break;

        default:
            abort();
    }
    return 0;
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each. The call back gets a
 * key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int art_iter(art_tree *t, art_callback cb, void *data) {
    return recursive_iter(t->root, cb, data);
}

/**
 * Checks if a leaf prefix matches
 * @return 0 on success.
 */
static int leaf_prefix_matches(const art_leaf *n, const unsigned char *prefix, int prefix_len) {
    // Fail if the key length is too short
    if (n->key_len < (uint32_t)prefix_len) return 1;

    // Compare the keys
    return memcmp(n->key, prefix, prefix_len);
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each that matches a given prefix.
 * The call back gets a key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg prefix The prefix of keys to read
 * @arg prefix_len The length of the prefix
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int art_iter_prefix(art_tree *t, const unsigned char *key, int key_len, art_callback cb, void *data) {
    art_node **child;
    art_node *n = t->root;
    int prefix_len, depth = 0;
    while (n) {
        // Might be a leaf
        if (IS_LEAF(n)) {
            n = (art_node*)LEAF_RAW(n);
            // Check if the expanded path matches
            if (!leaf_prefix_matches((art_leaf*)n, key, key_len)) {
                art_leaf *l = (art_leaf*)n;
                return cb(data, (const unsigned char*)l->key, l->key_len, l->value);
            }
            return 0;
        }

        // If the depth matches the prefix, we need to handle this node
        if (depth == key_len) {
            art_leaf *l = minimum(n);
            if (!leaf_prefix_matches(l, key, key_len))
               return recursive_iter(n, cb, data);
            return 0;
        }

        // Bail if the prefix does not match
        if (n->partial_len) {
            prefix_len = prefix_mismatch(n, key, key_len, depth);

            // Guard if the mis-match is longer than the MAX_PREFIX_LEN
            if ((uint32_t)prefix_len > n->partial_len) {
                prefix_len = n->partial_len;
            }

            // If there is no match, search is terminated
            if (!prefix_len) {
                return 0;

            // If we've matched the prefix, iterate on this node
            } else if (depth + prefix_len == key_len) {
                return recursive_iter(n, cb, data);
            }

            // if there is a full match, go deeper
            depth = depth + n->partial_len;
        }

        // Recursively search
        child = find_child(n, key[depth]);
        n = (child) ? *child : NULL;
        depth++;
    }
    return 0;
}
