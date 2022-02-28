// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

extern uint ticks;
#define HASHBUCKET 17
#define HASH(dev, blockno) (((dev) + (blockno)) % HASHBUCKET)
struct
{
  struct spinlock lock;
  struct buf buf[NBUF];
} bcache; // 缓冲区

struct
{
  struct spinlock hlock;
  struct spinlock lock[HASHBUCKET];
  // Linked list of all buffers when a hash conflict occurs.
  struct buf *head[HASHBUCKET];
} bhash;

void binit(void)
{
  int iticks = ticks;
  // 缓存池锁
  initlock(&bcache.lock, "bcache");
  for (struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    b->tick = iticks;
    initsleeplock(&b->lock, "bcache_buffer");
    (b < (bcache.buf + NBUF - 1)) ? (b->next = b + 1) : (b->next = 0);
  }

  // 哈希锁
  initlock(&bhash.hlock, "bcache_big_hash_lock");
  for (struct spinlock *b = bhash.lock; b < bhash.lock + HASHBUCKET; b++)
  {
    initlock(b, "bcache_bucket");
  }
  bhash.head[0] = &bcache.buf[0];
}

int printbucket(int bucket)
{
  struct buf *b;
  int num = 0;
  printf("    bucket[%d]: ", bucket);
  for (b = bhash.head[bucket]; b != 0; b = b->next)
  {
    printf("%p[%d|%d|%d]", b, b->refcnt, b->dev, b->blockno);
    printf(" -> ");
    num++;
  }
  printf("num %d", num);
  printf("\n");
  return num;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;

  int new_bucket = HASH(dev, blockno);
  acquire(&bhash.lock[new_bucket]);

  // 1 在 bhash 中查找
  // 2 如果 bhash 中存在缓存
  for (b = bhash.head[new_bucket]; b != 0; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      b->tick = ticks;
      release(&bhash.lock[new_bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bhash.lock[new_bucket]);

  acquire(&bhash.hlock);
  //   3 如果 bhash 中不存在缓存
  //   3.1 在 bhash 的每个 bucket 中搜索具有最小 time-stamp 的 cache，记录在 min_buf/min_bucket 中
  int bucket = new_bucket;
  for (int i = 0; i < HASHBUCKET; i++)
  {
    acquire(&bhash.lock[bucket]);

    struct buf *min_buf_pre = 0, *min_buf = 0, *b_pre = 0;
    uint minstamp = __UINT32_MAX__;
    for (b = bhash.head[bucket], b_pre = 0; b != 0;) // 表达式3会顺序执行吗？
    {
      if (bucket == new_bucket && b->dev == dev && b->blockno == blockno)
      {
        b->refcnt++;
        b->tick = ticks;
        release(&bhash.lock[bucket]);
        release(&bhash.hlock);
        acquiresleep(&b->lock);
        return b;
      }
      if (b->refcnt == 0 && b->tick < minstamp)
      {
        min_buf_pre = b_pre;
        min_buf = b;
        minstamp = b->tick;
      }
      b_pre = b;
      b = b->next;
    }
    if (min_buf)
    {
      min_buf->dev = dev;
      min_buf->blockno = blockno;
      min_buf->valid = 0;
      min_buf->refcnt = 1;
      min_buf->tick = ticks;
      if (bucket != new_bucket)
      {
        if (min_buf_pre)
          min_buf_pre->next = min_buf->next;
        else
          bhash.head[bucket] = min_buf->next;
        release(&bhash.lock[bucket]);

        acquire(&bhash.lock[new_bucket]);
        min_buf->next = bhash.head[new_bucket];
        bhash.head[new_bucket] = min_buf;
      }
      release(&bhash.lock[new_bucket]);
      release(&bhash.hlock);
      acquiresleep(&min_buf->lock);
      return min_buf;
    }

    release(&bhash.lock[bucket]);
    if (++bucket == HASHBUCKET)
      bucket = 0;
  }
  panic("ind: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bucket = HASH(b->dev, b->blockno);
  acquire(&bhash.lock[bucket]);
  b->refcnt--;
  if (b->refcnt == 0)
  {
    // no one is waiting for it.
    b->tick = ticks;
  }

  release(&bhash.lock[bucket]);
}

void bpin(struct buf *b)
{
  int bucket = HASH(b->dev, b->blockno);
  acquire(&bhash.lock[bucket]);
  b->refcnt++;
  release(&bhash.lock[bucket]);
}

void bunpin(struct buf *b)
{
  int bucket = HASH(b->dev, b->blockno);
  acquire(&bhash.lock[bucket]);
  b->refcnt--;
  release(&bhash.lock[bucket]);
}
