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

struct kmem
{
  struct spinlock lock;
  // int num;
  struct run *freelist;
} kmem[NCPU];

void kinit()
{
  for (int i = 0; i < NCPU; i++)
  {
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  push_off();
  int id = cpuid();
  pop_off();
  acquire(&kmem[id].lock);

  struct run *r;
  char *p;

  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
  {
    r = (struct run *)p;
    r->next = kmem[id].freelist;
    kmem[id].freelist = r;
    // kmem[id].num++;
  }
  // printf("cpu %d call freerange, %d free pages\n", id, kmem[id].num);
  release(&kmem[id].lock);
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

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  // kmem[id].num++;
  release(&kmem[id].lock);
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

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;

  if (!r)
  {
    // printf("cpu %d no free memory, have %d pages\n", id, kmem[id].num);
    // if (kmem[id].num != 0)
    //   panic("kalloc error\n");
    struct run * or ; // other run
    for (int i = 0; i < NCPU; i++)
    {

      if (i == id)
        continue;
      acquire(&kmem[i].lock);
      or = kmem[i].freelist;
      if (or)
      {
        struct run *fast = or, *pre = 0;
        while (fast && fast->next)
        {
          if (!pre)
            pre = or ;
          else
            pre = pre->next;
          fast = fast->next->next;
        }
        if (pre)
        {
          kmem[id].freelist = pre->next;
          pre->next = 0;
        }
        else
        {
          kmem[id].freelist = or ;
          kmem[i].freelist = 0;
        }
        // int ori = kmem[i].num;
        // kmem[id].num = 0;
        // kmem[i].num = 0;
        // struct run *test1 = kmem[id].freelist;
        // struct run *test2 = kmem[i].freelist;
        // while (test1)
        // {
        //   test1 = test1->next;
        //   kmem[id].num++;
        // }
        // while (test2)
        // {
        //   test2 = test2->next;
        //   kmem[i].num++;
        // }
        // printf("cpu[%d](%d/%d pages) ---> cpu[%d](%d pages)\n", i, ori, kmem[i].num, id, kmem[id].num);

        r = kmem[id].freelist;
        release(&kmem[i].lock);
        break;
      }
      release(&kmem[i].lock);
    }
  }
  if (r)
    kmem[id].freelist = r->next;

  // kmem[id].num--;
  release(&kmem[id].lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}
