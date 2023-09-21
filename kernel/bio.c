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

#define BUCKETSIZE 13 

struct {
  struct spinlock lock[BUCKETSIZE];
  struct spinlock find;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // 单向链表
  struct buf head[BUCKETSIZE];
} bcache;

struct buf evict;

void
binit(void)
{
  // printf(" init BEGIN\n ");
  struct buf *b;

  initlock(&bcache.find, "find");

  for(int i=0; i<BUCKETSIZE; i++){
    initlock(&bcache.lock[i], "bcache");
    bcache.head[i].next = 0;
  }

  // Create linked list of buffers
  // 环形
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->refcnt = 0;
    b->tick = 0;
    b->next = evict.next;
    evict.next = b;
  }
  // printf(" init end\n ");
}

// 获取最久未使用的buf
// 记得释放锁
int 
get_oldbuf (struct buf ** res) {
  // printf(" get_oldbuf here begin\n ");
  uint old = 4294967295U;
  int j=-1;
  int flag = 0;
  int sum=0;
  for(int i=0 ; i<BUCKETSIZE; i++) {
    acquire(&bcache.lock[i]);
    // printf(" get_oldbuf here %d\n ", i);
    flag = 0;
    
    struct buf * pre = &bcache.head[i];
    struct buf * stash;
    for(struct buf * b = bcache.head[i].next; b; b = b->next) {
      sum++;
      // printf(" get_oldbuf here buf %d\n ", flag);
      // 一旦选中就转移到head位置,pre位置继续往后遍历
      if(b->refcnt == 0 && b->tick < old) {
        stash = pre;
        flag = 1;
        // printf(" get_oldbuf here renew %d %p\n",i,b);
        pre->next = b->next;
        b->next = bcache.head[i].next;
        bcache.head[i].next = b;

        *res = b;
        old = b->tick;
        b = stash;
      }
      pre = b;
    }
    // 如果没拿到，锁就直接释放
    if(!flag) release(&bcache.lock[i]);
    else {
      if(j != -1)release(&bcache.lock[j]);
      j = i;
    }
  }
  printf(" get_oldbuf here end %d\n ",sum);

  return j;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  // printf(" bget begin\n ");
  struct buf *b;

  uint hash = blockno % BUCKETSIZE;
  acquire(&bcache.lock[hash]);

  // Is the block already cached?
  printf(" bget here %d\n ",hash);
  for(b = bcache.head[hash].next; b; b = b->next){
    // printf(" bget loop\n ");
    if(b->dev == dev && b->blockno == blockno){
      // printf(" bget 001\n ");
      b->refcnt++;
      // printf(" bget 001\n ");
      b->tick = ticks;
      // printf(" bget 001\n ");
      release(&bcache.lock[hash]);
      // printf(" bget 001\n ");
      acquiresleep(&b->lock);
      // printf(" bget 001\n ");
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  printf(" bget here\n ");
  release(&bcache.lock[hash]);
  int id = get_oldbuf(&b);

  printf(" bget id %d %p\n ",id, b);
  if(id != -1) {
    // // printf(" bget 001\n ");
    if (id != hash) acquire(&bcache.lock[hash]);
    struct buf *bf = b;
    if (id != hash) {
      bcache.head[id].next = b->next;
      release(&bcache.lock[id]);
    } 

    // // printf(" bget 002\n ");
    bf->dev = dev;
    bf->blockno = blockno;
    bf->valid = 0;
    bf->refcnt = 1;

    // // printf(" bget 003\n ");
    // 转移到新的桶头
    if (id != hash) {
      bf->next = bcache.head[0].next;
      bcache.head[0].next = bf;
    }
    initsleeplock(&bf->lock, "buffer");
    
    release(&bcache.lock[hash]);
    acquiresleep(&b->lock);
    // printf(" bget 004\n ");
    return b;
  }

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int hash = b->blockno % BUCKETSIZE;
  acquire(&bcache.lock[hash]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->tick = ticks;
  }
  
  release(&bcache.lock[hash]);
}

void
bpin(struct buf *b) {
  int hash = b->blockno % BUCKETSIZE;

  acquire(&bcache.lock[hash]);
  b->refcnt++;
  release(&bcache.lock[hash]);
}

void
bunpin(struct buf *b) {
  int hash = b->blockno % BUCKETSIZE;

  acquire(&bcache.lock[hash]);
  b->refcnt--;
  release(&bcache.lock[hash]);
}


