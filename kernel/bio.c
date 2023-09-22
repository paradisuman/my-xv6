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
  struct buf buf[NBUF];

  // 单向链表
  struct buf head[BUCKETSIZE];
} bcache;

// 共享但是简短的lru可用 buf
struct 
{
  struct buf* buf[40];
  int begin, end;
}evict;

struct spinlock elock;

void
binit(void)
{
  struct buf *b;

  initlock(&elock, "elock");
  evict.begin = evict.end = 0;

  for(int i=0; i<BUCKETSIZE; i++){
    initlock(&bcache.lock[i], "bcache");
    bcache.head[i].next = 0;
  }

  // Create linked list of buffers
  // 放入evict环
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->refcnt = 0;

    evict.buf[evict.end] = b;
    evict.end = (evict.end + 1) % 40;
  }
}

// 获取最久未使用的buf,并且移除evict
// 记得释放锁
void
get_oldbuf (struct buf ** res) {
  acquire(&elock);
  if(evict.begin != evict.end) {
    *res = evict.buf[evict.begin];
    evict.begin = (evict.begin + 1) % 40;
  }
  release(&elock);
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint hash = blockno % BUCKETSIZE;
  acquire(&bcache.lock[hash]);

  // Is the block already cached?
  for(b = bcache.head[hash].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[hash]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  get_oldbuf(&b);

  // 获取到
  if(b) {
    // 移动到头部
    b->next = bcache.head[hash].next;
    bcache.head[hash].next = b;

    b->dev = dev, b->blockno = blockno;
    b->valid = 0, b->refcnt = 1; 

    release(&bcache.lock[hash]);
    acquiresleep(&b->lock);
    return b;
  }

  release(&bcache.lock[hash]);
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
    // 删除
    struct buf *pre = &bcache.head[hash];
    while(pre->next != b){
      pre = pre->next;
    }
    pre->next = b->next;
    acquire(&elock);
    evict.buf[evict.end] = b;
    evict.end = (evict.end + 1) % 40;
    release(&elock);
  }
  
  
  // printf("reles: sum is %d\n",sum);
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


