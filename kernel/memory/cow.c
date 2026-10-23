/* HarisOS copy-on-write page counts: see include/cow.h. An open-addressing hash table (linear probing, deletion by shifting the
 * following entries back) from page number to the number of holders beyond the first. */
#include "cow.h"

typedef unsigned long long u64;

static struct
{
    u64 key;        /* page number + 1; 0 = free slot */
    unsigned extra; /* holders besides the first: always >= 1 while the entry exists */
} TABLE[COW_TABLE_SIZE];
static unsigned USED;

static unsigned home(u64 key) { return (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 48) & (COW_TABLE_SIZE - 1); }

/* slot of the page, or -1 */
static int find(u64 key)
{
    unsigned i = home(key);
    for (unsigned n = 0; n < COW_TABLE_SIZE; n++, i = (i + 1) & (COW_TABLE_SIZE - 1))
    {
        if (TABLE[i].key == key)
            return (int)i;
        if (TABLE[i].key == 0)
            return -1;
    }
    return -1;
}

int cow_shared(u64 phys) { return find((phys >> 12) + 1) >= 0; }

unsigned cow_shared_pages(void) { return USED; }

int cow_share(u64 phys)
{
    u64 key = (phys >> 12) + 1;
    int s = find(key);
    if (s >= 0)
    {
        TABLE[s].extra++;
        return 0;
    }
    if (USED >= COW_TABLE_SIZE - COW_TABLE_SIZE / 8)
        return -1; /* keep the table a little empty: probing stays short */
    unsigned i = home(key);
    while (TABLE[i].key != 0)
        i = (i + 1) & (COW_TABLE_SIZE - 1);
    TABLE[i].key = key;
    TABLE[i].extra = 1;
    USED++;
    return 0;
}

int cow_release(u64 phys)
{
    int s = find((phys >> 12) + 1);
    if (s < 0)
        return 1; /* held by one: the caller was it */
    if (--TABLE[s].extra > 0)
        return 0;
    /* one holder is left: the page leaves the table. Close the gap: entries that follow and could have lived at or before it move back. */
    unsigned i = (unsigned)s;
    TABLE[i].key = 0;
    USED--;
    unsigned j = i;
    for (;;)
    {
        j = (j + 1) & (COW_TABLE_SIZE - 1);
        if (TABLE[j].key == 0)
            break;
        unsigned h = home(TABLE[j].key);
        /* does the entry at j belong at or before i (cyclically)? then it moves into the gap */
        int movable = i <= j ? (h <= i || h > j) : (h <= i && h > j);
        if (movable)
        {
            TABLE[i] = TABLE[j];
            TABLE[j].key = 0;
            i = j;
        }
    }
    return 0;
}
