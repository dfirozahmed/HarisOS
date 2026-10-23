/* Host test for gui.c: surfaces, their pixel segments, damage, the event queue, close requests and handles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui.h"
#include "pmm.h"
#include "shm.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* a fake page allocator: real host memory, counted */
static int PAGES_OUT, PAGE_LIMIT = 100000;
unsigned long long pmm_alloc_page_tag(PmmTag tag)
{
    (void)tag;
    if (PAGES_OUT >= PAGE_LIMIT)
        return 0;
    void *p = calloc(1, 4096);
    if (!p)
        return 0;
    PAGES_OUT++;
    return (unsigned long long)p;
}

void pmm_free_page(unsigned long long phys)
{
    free((void *)phys);
    PAGES_OUT--;
}

/* the kernel heap (shm.c keeps its page lists there, gui.c its surfaces): host memory, counted, with a switch that makes it fail */
static int HEAP_BLOCKS, HEAP_FAIL;
static int TABLES; /* heap blocks the segment table (and the surface table) keep for good once they exist */
void *kmalloc(unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *p = calloc(1, size ? (size_t)size : 1);
    if (p)
        HEAP_BLOCKS++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        free(p);
        HEAP_BLOCKS--;
    }
}
void *krealloc(void *p, unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *n = realloc(p, size ? (size_t)size : 1);
    if (n && !p)
        HEAP_BLOCKS++;
    return n;
}

static int WOKEN[16], WOKEN_N;
void task_wake(int id)
{
    if (WOKEN_N < 16)
        WOKEN[WOKEN_N++] = id;
}

static GuiEvent key_ev(int key)
{
    GuiEvent e = {GUI_EV_KEY, 0, 0, 0, key, key, 1, 0};
    return e;
}

static GuiEvent move_ev(int x, int y)
{
    GuiEvent e = {GUI_EV_MOUSE_MOVE, x, y, 0, 0, 0, 0, 0};
    return e;
}

int main(void)
{
    int seg = -1, w, h, x, y, n;

    /* the tables are made on first use and stay: count them once, so the leak checks below can ask for "nothing else" */
    int warm = gui_create("Warm", 64, 48, 1, &seg);
    CHECK(warm > 0 && gui_slot_count() >= 1);
    gui_destroy(warm);
    TABLES = HEAP_BLOCKS;

    /* create: size limits, titles, the pixel segment */
    CHECK(gui_create("Hello", GUI_W_MIN - 1, 100, 7, &seg) == -1);
    CHECK(gui_create("Hello", 100, GUI_H_MIN - 1, 7, &seg) == -1);
    CHECK(gui_create("Hello", GUI_W_MAX + 1, 100, 7, &seg) == -1);
    CHECK(gui_create("Hello", 100, GUI_H_MAX + 1, 7, &seg) == -1);
    CHECK(gui_create("", 100, 100, 7, &seg) == -1);
    CHECK(gui_create("tab\there", 100, 100, 7, &seg) == -1);
    char longt[64];
    memset(longt, 'T', sizeof(longt));
    longt[GUI_TITLE_MAX] = 0;
    CHECK(gui_create(longt, 100, 100, 7, &seg) == -1);
    longt[GUI_TITLE_MAX - 1] = 0;
    CHECK(gui_in_use() == 0 && shm_in_use() == 0 && PAGES_OUT == 0); /* the failures left nothing behind */

    int a = gui_create("Hello", 100, 60, 7, &seg), segA = seg;
    CHECK(a > 0 && gui_valid(a) && !gui_valid(-1) && !gui_valid(0) && gui_owner(a) == 7);
    CHECK(seg >= 0 && shm_pages(seg) == 6 && PAGES_OUT == 6 && gui_in_use() == 1); /* 100*60*4 = 24000 bytes: 6 pages */
    CHECK(shm_open("gui.0.1", 0, 0) == -1);                                        /* the name is gone, the segment is held */
    CHECK(gui_size(a, &w, &h) && w == 100 && h == 60 && strcmp(gui_title(a), "Hello") == 0);
    int t31 = gui_create(longt, 64, 48, 7, &seg);
    CHECK(t31 >= 0); /* a 31 character title is fine */
    gui_destroy(t31);
    CHECK(gui_in_use() == 1);
    seg = segA;

    /* a new surface shows its first picture; the rectangle is handed out once */
    CHECK(gui_take_dirty(a, &x, &y, &w, &h) && x == 0 && y == 0 && w == 100 && h == 60);
    CHECK(!gui_take_dirty(a, &x, &y, &w, &h));

    /* present: clipped, joined, "all" for an empty rectangle, nothing for a rectangle outside */
    CHECK(gui_present(a, 10, 10, 20, 20) == 0);
    CHECK(gui_present(a, 50, 5, 10, 10) == 0);
    CHECK(gui_take_dirty(a, &x, &y, &w, &h) && x == 10 && y == 5 && w == 50 && h == 25);
    CHECK(gui_present(a, -20, -20, 30, 30) == 0);
    CHECK(gui_take_dirty(a, &x, &y, &w, &h) && x == 0 && y == 0 && w == 10 && h == 10);
    CHECK(gui_present(a, 90, 50, 500, 500) == 0);
    CHECK(gui_take_dirty(a, &x, &y, &w, &h) && x == 90 && y == 50 && w == 10 && h == 10);
    CHECK(gui_present(a, 0, 0, 0, 0) == 0);
    CHECK(gui_take_dirty(a, &x, &y, &w, &h) && w == 100 && h == 60);
    CHECK(gui_present(a, 200, 200, 5, 5) == 0 && !gui_take_dirty(a, &x, &y, &w, &h));                 /* outside: nothing to show */
    CHECK(gui_present(a, 0x7FFFFFF0, 0, 0x7FFFFFF0, 10) == 0 && !gui_take_dirty(a, &x, &y, &w, &h)); /* x + w must not wrap */
    CHECK(gui_present(-1, 0, 0, 1, 1) == -1);

    /* the pixels: runs end at the row's end or at the page's end */
    CHECK(seg >= 0);
    unsigned *p0 = (unsigned *)shm_page(seg, 0);
    unsigned *p1 = (unsigned *)shm_page(seg, 1);
    p0[5] = 0x11223344;
    p1[0] = 0xAABBCCDD; /* pixel 1024 = row 10, column 24 */
    const unsigned *r = gui_pixels(a, 5, 0, &n);
    CHECK(r == p0 + 5 && n == 95 && *r == 0x11223344);
    r = gui_pixels(a, 0, 10, &n);
    CHECK(r == p0 + 1000 && n == 24); /* page 0 holds 1024 pixels: 24 of row 10 */
    r = gui_pixels(a, 24, 10, &n);
    CHECK(r == p1 && n == 76 && *r == 0xAABBCCDD);
    CHECK(gui_pixels(a, 100, 0, &n) == 0 && gui_pixels(a, 0, 60, &n) == 0 && gui_pixels(a, -1, 0, &n) == 0);

    /* events: order, boundaries, a waiting reader, mouse moves replace each other */
    GuiEvent e;
    CHECK(gui_next_event(a, &e) == 0);
    gui_wait(a, 5);
    WOKEN_N = 0;
    GuiEvent k = key_ev('a');
    CHECK(gui_post(a, &k) == 1 && WOKEN_N == 1 && WOKEN[0] == 5);
    k = key_ev('b');
    CHECK(gui_post(a, &k) == 1 && WOKEN_N == 1); /* nobody is registered any more */
    GuiEvent m = move_ev(1, 2);
    CHECK(gui_post(a, &m) == 1);
    m = move_ev(3, 4);
    CHECK(gui_post(a, &m) == 1); /* replaces the waiting move */
    k = key_ev('c');
    CHECK(gui_post(a, &k) == 1);
    m = move_ev(5, 6);
    CHECK(gui_post(a, &m) == 1); /* a key lies between: a new entry */
    CHECK(gui_next_event(a, &e) == 1 && e.type == GUI_EV_KEY && e.key == 'a');
    CHECK(gui_next_event(a, &e) == 1 && e.type == GUI_EV_KEY && e.key == 'b');
    CHECK(gui_next_event(a, &e) == 1 && e.type == GUI_EV_MOUSE_MOVE && e.x == 3 && e.y == 4);
    CHECK(gui_next_event(a, &e) == 1 && e.type == GUI_EV_KEY && e.key == 'c');
    CHECK(gui_next_event(a, &e) == 1 && e.type == GUI_EV_MOUSE_MOVE && e.x == 5 && e.y == 6);
    CHECK(gui_next_event(a, &e) == 0);

    /* a full queue drops new events, but never a close request */
    for (int i = 0; i < GUI_EVENTS; i++)
    {
        k = key_ev('a' + i % 26);
        CHECK(gui_post(a, &k) == 1);
    }
    k = key_ev('z');
    CHECK(gui_post(a, &k) == 0);
    CHECK(gui_request_close(a) == 1);
    int closes = 0, total = 0;
    while (gui_next_event(a, &e) == 1)
    {
        total++;
        closes += e.type == GUI_EV_CLOSE;
    }
    CHECK(closes == 1 && total == GUI_EVENTS);

    /* close requests: one at a time; a second one before the client read the first means it is stuck */
    CHECK(gui_request_close(a) == 1);
    CHECK(gui_request_close(a) == 0);
    CHECK(gui_next_event(a, &e) == 1 && e.type == GUI_EV_CLOSE);
    CHECK(gui_request_close(a) == 1); /* it was read: asking again is fine */
    CHECK(gui_next_event(a, &e) == 1);
    CHECK(gui_request_close(-1) == 0);

    /* titles */
    CHECK(!gui_take_title_changed(a));
    CHECK(gui_set_title(a, "New name") == 0 && strcmp(gui_title(a), "New name") == 0);
    CHECK(gui_take_title_changed(a) && !gui_take_title_changed(a));
    CHECK(gui_set_title(a, "") == -1 && gui_set_title(a, longt) == 0 && gui_set_title(-1, "x") == -1);
    longt[GUI_TITLE_MAX - 1] = 'T';
    longt[GUI_TITLE_MAX] = 0;
    CHECK(gui_set_title(a, longt) == -1);

    /* destroy: the handle dies, a waiting client is woken, the pixels live while the client still maps them */
    shm_ref(seg); /* "the client's mapping" */
    gui_wait(a, 9);
    WOKEN_N = 0;
    gui_destroy(a);
    CHECK(!gui_valid(a) && WOKEN_N == 1 && WOKEN[0] == 9 && gui_in_use() == 0);
    CHECK(gui_next_event(a, &e) == -1 && gui_present(a, 0, 0, 1, 1) == -1 && gui_pixels(a, 0, 0, &n) == 0);
    CHECK(shm_pages(seg) == 6 && PAGES_OUT == 6);
    shm_unref(seg); /* the client unmaps */
    CHECK(shm_in_use() == 0 && PAGES_OUT == 0);
    gui_destroy(a); /* twice is harmless */

    /* a handle left over from a destroyed surface never reaches the surface that took its slot */
    int b = gui_create("B", 64, 48, 8, &seg);
    CHECK(b > 0 && b != a && gui_slot(b) == (a & 255) && gui_slot(a) == -1 && !gui_valid(a) && gui_valid(b));
    CHECK(gui_post(a, &k) == -1);
    gui_destroy(b);
    CHECK(HEAP_BLOCKS - TABLES == 0 && gui_in_use() == 0); /* the surface and the page list went back to the heap */

    /* the slots: GUI_MAX at most (all small), and a process that ends takes its surfaces with it */
    int hs[GUI_MAX];
    hs[0] = gui_create("C", 64, 48, 9, &seg);
    for (int i = 1; i < GUI_MAX; i++)
        hs[i] = gui_create("D", 64, 48, 8, &seg);
    int all = 1;
    for (int i = 0; i < GUI_MAX; i++)
        all = all && hs[i] > 0 && gui_slot(hs[i]) == i && gui_handle_at(i) == hs[i];
    CHECK(all && gui_in_use() == GUI_MAX);
    CHECK(gui_slot_count() == GUI_MAX); /* the table grew to the limit, by doubling */
    CHECK(gui_create("F", 64, 48, 8, &seg) == -1);
    CHECK(HEAP_BLOCKS - TABLES == 2 * GUI_MAX && shm_in_use() == GUI_MAX); /* a surface and a page list each */
    gui_release_owner(8);
    all = 1;
    for (int i = 1; i < GUI_MAX; i++)
        all = all && !gui_valid(hs[i]);
    CHECK(all && gui_in_use() == 1 && gui_valid(hs[0]) && HEAP_BLOCKS - TABLES == 2);
    CHECK(gui_handle_at(0) == hs[0] && gui_handle_at(1) == -1 && gui_handle_at(-1) == -1 && gui_handle_at(GUI_MAX) == -1);
    gui_release_owner(9);
    CHECK(gui_in_use() == 0 && shm_in_use() == 0 && PAGES_OUT == 0 && HEAP_BLOCKS - TABLES == 0);

    /* out of memory half way (pages, or the heap): nothing stays allocated */
    PAGE_LIMIT = 1;
    CHECK(gui_create("Big", 512, 512, 1, &seg) == -1);
    PAGE_LIMIT = 100000;
    HEAP_FAIL = 1;
    CHECK(gui_create("Big", 512, 512, 1, &seg) == -1);
    HEAP_FAIL = 0;
    CHECK(gui_in_use() == 0 && shm_in_use() == 0 && PAGES_OUT == 0 && HEAP_BLOCKS - TABLES == 0);
    int big = gui_create("Big", GUI_W_MAX, GUI_H_MAX, 1, &seg);
    CHECK(big > 0 && shm_pages(seg) == (unsigned)((GUI_W_MAX * GUI_H_MAX * 4 + 4095) / 4096)); /* the largest surface still fits a segment */
    CHECK(shm_pages(seg) <= SHM_MAX_PAGES);
    gui_destroy(big);
    CHECK(PAGES_OUT == 0 && HEAP_BLOCKS - TABLES == 0);

    if (fails)
    {
        printf("%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("gui tests passed\n");
    return 0;
}
