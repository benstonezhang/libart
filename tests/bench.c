#include <sys/time.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art.h"

const unsigned char long_key1[] = {
    16, 0, 0, 0, 7, 10, 0, 0, 0, 2, 17, 10, 0, 0, 0, 120, 10, 0, 0, 0, 120, 10, 0,
    0, 0, 216, 10, 0, 0, 0, 202, 10, 0, 0, 0, 194, 10, 0, 0, 0, 224, 10, 0, 0, 0,
    230, 10, 0, 0, 0, 210, 10, 0, 0, 0, 206, 10, 0, 0, 0, 208, 10, 0, 0, 0, 232,
    10, 0, 0, 0, 124, 10, 0, 0, 0, 124, 2, 16, 0, 0, 0, 2, 12, 185, 89, 44, 213,
    251, 173, 202, 211, 95, 185, 89, 110, 118, 251, 173, 202, 199, 101, 0,
    8, 18, 182, 92, 236, 147, 171, 101, 150, 195, 112, 185, 218, 108, 246,
    139, 164, 234, 195, 58, 177, 0, 8, 16, 0, 0, 0, 2, 12, 185, 89, 44, 213,
    251, 173, 202, 211, 95, 185, 89, 110, 118, 251, 173, 202, 199, 101, 0,
    8, 18, 180, 93, 46, 151, 9, 212, 190, 95, 102, 178, 217, 44, 178, 235,
    29, 190, 218, 8, 16, 0, 0, 0, 2, 12, 185, 89, 44, 213, 251, 173, 202,
    211, 95, 185, 89, 110, 118, 251, 173, 202, 199, 101, 0, 8, 18, 180, 93,
    46, 151, 9, 212, 190, 95, 102, 183, 219, 229, 214, 59, 125, 182, 71,
    108, 180, 220, 238, 150, 91, 117, 150, 201, 84, 183, 128, 8, 16, 0, 0,
    0, 2, 12, 185, 89, 44, 213, 251, 173, 202, 211, 95, 185, 89, 110, 118,
    251, 173, 202, 199, 101, 0, 8, 18, 180, 93, 46, 151, 9, 212, 190, 95,
    108, 176, 217, 47, 50, 219, 61, 134, 207, 97, 151, 88, 237, 246, 208,
    8, 18, 255, 255, 255, 219, 191, 198, 134, 5, 223, 212, 72, 44, 208,
    250, 180, 14, 1, 0, 0, 8, '\0'};
const unsigned char long_key2[] = {
    16, 0, 0, 0, 7, 10, 0, 0, 0, 2, 17, 10, 0, 0, 0, 120, 10, 0, 0, 0, 120, 10, 0,
    0, 0, 216, 10, 0, 0, 0, 202, 10, 0, 0, 0, 194, 10, 0, 0, 0, 224, 10, 0, 0, 0,
    230, 10, 0, 0, 0, 210, 10, 0, 0, 0, 206, 10, 0, 0, 0, 208, 10, 0, 0, 0, 232,
    10, 0, 0, 0, 124, 10, 0, 0, 0, 124, 2, 16, 0, 0, 0, 2, 12, 185, 89, 44, 213,
    251, 173, 202, 211, 95, 185, 89, 110, 118, 251, 173, 202, 199, 101, 0,
    8, 18, 182, 92, 236, 147, 171, 101, 150, 195, 112, 185, 218, 108, 246,
    139, 164, 234, 195, 58, 177, 0, 8, 16, 0, 0, 0, 2, 12, 185, 89, 44, 213,
    251, 173, 202, 211, 95, 185, 89, 110, 118, 251, 173, 202, 199, 101, 0,
    8, 18, 180, 93, 46, 151, 9, 212, 190, 95, 102, 178, 217, 44, 178, 235,
    29, 190, 218, 8, 16, 0, 0, 0, 2, 12, 185, 89, 44, 213, 251, 173, 202,
    211, 95, 185, 89, 110, 118, 251, 173, 202, 199, 101, 0, 8, 18, 180, 93,
    46, 151, 9, 212, 190, 95, 102, 183, 219, 229, 214, 59, 125, 182, 71,
    108, 180, 220, 238, 150, 91, 117, 150, 201, 84, 183, 128, 8, 16, 0, 0,
    0, 3, 12, 185, 89, 44, 213, 251, 133, 178, 195, 105, 183, 87, 237, 150,
    155, 165, 150, 229, 97, 182, 0, 8, 18, 161, 91, 239, 50, 10, 61, 150,
    223, 114, 179, 217, 64, 8, 12, 186, 219, 172, 150, 91, 53, 166, 221,
    101, 178, 0, 8, 18, 255, 255, 255, 219, 191, 198, 134, 5, 208, 212, 72,
    44, 208, 250, 180, 14, 1, 0, 0, 8, '\0'};

unsigned value1 = 1, value2 = 2, value3 = 3;

typedef struct {
    unsigned char *s;
    size_t len;
} word_info;

int iter_cb(void *data, const unsigned char *key, uint32_t key_len, void *val) {
    uint64_t *out = (uint64_t *)data;
    uintptr_t line = (uintptr_t)val;
    uint64_t mask = (line * (key[0] + key_len));
    out[0]++;
    out[1] ^= mask;
    return 0;
}

static uintptr_t val_sum = 0;
static int loop = 100;

static int test_prefix_cb(void *data, const unsigned char *k, uint32_t k_len, void *val) {
    (void)data;
    val_sum += *k + k_len + (uintptr_t)val;
    return 0;
}

int main() {
    art_tree t;
    int len;
    off_t off;
    word_info *ws;
    unsigned char *str;
    uintptr_t line;
    art_leaf *l;
    char buf[512];
    struct timeval tv;
    unsigned long long ts;

    size_t total_len = 0;

    FILE *f1 = fopen("tests/words.txt", "r");
    FILE *f2 = fopen("tests/uuid.txt", "r");
    int count = 0;
    while (fgets(buf, sizeof buf, f1)) {
        len = strlen(buf);
        total_len += len;
        count++;
    }
    while (fgets(buf, sizeof buf, f2)) {
        len = strlen(buf);
        total_len += len;
        count++;
    }
    int total = count;
    printf("read %d keys\n", total);

    ws = (word_info *)malloc(sizeof(word_info) * count + total_len);
    str = ((unsigned char *)ws) + sizeof(word_info) * count;

    count = 0;
    off = 0;

    fseek(f1, 0, SEEK_SET);
    while (fgets(buf, sizeof buf, f1)) {
        len = strlen(buf);
        buf[len - 1] = '\0';
        ws[count].s = str + off;
        ws[count].len = len;
        memcpy(ws[count].s, buf, len);
        count++;
        off += len;
    }

    fseek(f2, 0, SEEK_SET);
    while (fgets(buf, sizeof buf, f2)) {
        len = strlen(buf);
        buf[len - 1] = '\0';
        ws[count].s = str + off;
        ws[count].len = len;
        memcpy(ws[count].s, buf, len);
        count++;
        off += len;
    }

    fclose(f1);
    fclose(f2);

    gettimeofday(&tv, NULL);
    ts = ((long)tv.tv_sec) * 1000000 + tv.tv_usec;

    for (int iter = 0; iter < loop; iter++) {
        art_tree_init(&t);

        for (count = 0, line = 1; count < total; count++, line++)
            art_insert(&t, ws[count].s, ws[count].len, (void *)line);

        l = art_minimum(&t);
        val_sum += (uintptr_t)l->value;
        l = art_maximum(&t);
        val_sum += (uintptr_t)l->value;

        for (int i = 0; i < 3; i++) {
            for (count = 0; count < total; count++)
                art_search(&t, ws[count].s, ws[count].len);

            val_sum += (uintptr_t)art_search(&t, long_key1, sizeof(long_key1));
            val_sum += (uintptr_t)art_search(&t, long_key2, sizeof(long_key2));
        }

        for (count = 0; count < total; count++)
            art_delete(&t, ws[count].s, ws[count].len);

        for (count = 0; count < total; count++, line++)
            art_insert(&t, ws[count].s, ws[count].len, (void *)line);

        uint64_t out[] = {0, 0};
        art_iter(&t, iter_cb, &out);

        const char *s = "api.foo.bar";
        art_insert(&t, (unsigned char *)s, strlen(s), NULL);
        s = "api.foo.baz";
        art_insert(&t, (unsigned char *)s, strlen(s), NULL);
        s = "api.foe.fum";
        art_insert(&t, (unsigned char *)s, strlen(s), NULL);
        s = "abc.123.456";
        art_insert(&t, (unsigned char *)s, strlen(s), NULL);
        s = "api.foo";
        art_insert(&t, (unsigned char *)s, strlen(s), NULL);
        s = "api";
        art_insert(&t, (unsigned char *)s, strlen(s), NULL);

        art_iter_prefix(&t, (unsigned char *)"api", 3, test_prefix_cb, NULL);
        art_iter_prefix(&t, (unsigned char *)"a", 1, test_prefix_cb, NULL);
        art_iter_prefix(&t, (unsigned char *)"api.", 4, test_prefix_cb, NULL);
        art_iter_prefix(&t, (unsigned char *)"api.foo.bar", 11, test_prefix_cb, NULL);
        art_iter_prefix(&t, (unsigned char *)"api.end", 7, test_prefix_cb, NULL);

        s = "this:key:has:a:long:prefix:3";
        art_insert(&t, (unsigned char *)s, strlen(s) + 1, (void *)&value1);
        s = "this:key:has:a:long:common:prefix:2";
        art_insert(&t, (unsigned char *)s, strlen(s) + 1, (void *)&value2);
        s = "this:key:has:a:long:common:prefix:1";
        art_insert(&t, (unsigned char *)s, strlen(s) + 1, (void *)&value3);

        // Search for the keys
        s = "this:key:has:a:long:common:prefix:1";
        art_search(&t, (unsigned char *)s, strlen(s) + 1);
        s = "this:key:has:a:long:common:prefix:2";
        art_search(&t, (unsigned char *)s, strlen(s) + 1);
        s = "this:key:has:a:long:prefix:3";
        art_search(&t, (unsigned char *)s, strlen(s) + 1);
        art_iter_prefix(&t, (unsigned char *)"this:key:has", 12, test_prefix_cb, NULL);
        char *foo1 = "foobarbaz1-test1-foo";
        art_insert(&t, (unsigned char *)foo1, strlen(foo1) + 1, NULL);
        char *foo2 = "foobarbaz1-test1-bar";
        art_insert(&t, (unsigned char *)foo2, strlen(foo2) + 1, NULL);
        char *foo3 = "foobarbaz1-test2-foo";
        art_insert(&t, (unsigned char *)foo3, strlen(foo3) + 1, NULL);
        char *prefix = "foobarbaz1-test1";
        art_iter_prefix(&t, (unsigned char *)prefix, strlen(prefix), test_prefix_cb, NULL);

        for (count = 0; count < total; count++)
            art_delete(&t, ws[count].s, ws[count].len);

        art_tree_destroy(&t);
    }

    gettimeofday(&tv, NULL);
    ts = ((int64_t)tv.tv_sec) * 1000000 + tv.tv_usec - ts;
    printf("time: %f nanoseconds [%f seconds for %d loops]\n",
           ((double)ts) * 1000 / loop, ts * 1e-6, loop);

    return val_sum >> 24;
}
