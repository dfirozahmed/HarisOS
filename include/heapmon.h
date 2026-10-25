/* ============================================================
 * HarisOS heap monitor: a background kernel thread that checks the kernel heap once a second
 *
 *   heapmon_start();        once, after task_init() and heap_init(); -1 if the thread could not be made
 *   heapmon_checks();       how many checks have run
 *   heapmon_last_result();  what the last heap_check() said (0 = fine)
 *
 * Why: a bug that writes past a kmalloc block damages the block headers, and the crash comes much later, far from
 * the cause. Checking the whole heap regularly (heap_check(), see heap.h) finds the damage within a second, and the
 * message on the serial port says the heap is the problem.
 *
 * It is the first real user of a preemptible thread (task.h): it only uses the heap, which locks itself with
 * a spinlock (heap.c), so the check always sees the heap between two operations, never in the middle of one.
 * ============================================================ */
#ifndef HARIS_HEAPMON_H
#define HARIS_HEAPMON_H

int heapmon_start(void);
unsigned heapmon_checks(void);
int heapmon_last_result(void);

#endif
