// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
  struct run *next;
};

struct
{
  struct spinlock klock;
  // int num;
  struct run *freelist[NCPU];
  struct spinlock lock[NCPU];
} kmem;

void kinit()
{
  initlock(&kmem.klock, "kmem_klock");
  for (int i = 0; i < NCPU; i++)
    initlock(&kmem.lock[i], "kmem");

  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  push_off();
  int id = cpuid();
  pop_off();
  acquire(&kmem.lock[id]);

  struct run *r;
  char *p;

  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
  {
    r = (struct run *)p;
    r->next = kmem.freelist[id];
    kmem.freelist[id] = r;
  }
  release(&kmem.lock[id]);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem.lock[id]);
  r->next = kmem.freelist[id];
  kmem.freelist[id] = r;
  // kmem.num[id]++;
  release(&kmem.lock[id]);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem.lock[id]);
  r = kmem.freelist[id];

  if (!r)
  {
    // 先释放，防止死锁——即本线程持AB锁，其它线程持BA锁的情况
    release(&kmem.lock[id]);
    struct run * or ; // other run
    int i = id;
    for (int j = 0; j < NCPU; j++)
    {
      acquire(&kmem.lock[i]);
      or = kmem.freelist[i];
      if (or)
      {
        struct run *fast = or, *pre = 0;
        while (fast && fast->next)
        {
          (0 != pre) ? (pre = pre->next) : (pre = or);
          fast = fast->next->next;
        }
        if (pre)
        {
          r = pre->next;
          pre->next = 0;
        }
        else
        {
          r = or ;
          kmem.freelist[i] = 0;
        }
        release(&kmem.lock[i]);
        break;
      }
      release(&kmem.lock[i]);
      if (++i == NCPU)
        i = 0;
    }
    acquire(&kmem.lock[id]);
  }

  if (r)
    kmem.freelist[id] = r->next;

  release(&kmem.lock[id]);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}
