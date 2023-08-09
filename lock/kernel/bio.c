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

#define NBUK  13
#define hash(dev, blockno)  ((dev * blockno) % NBUK) // we use "mod" to establish func of hash

struct bucket{
  struct spinlock lock;
  struct buf head; // the head of current bucket
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF]; // cache有30个buffer
  struct bucket buckets[NBUK]; // 13个散列桶
} bcache;

void
binit(void)
{
  struct buf *b;
  struct buf *prev_b;
  //bache锁的初始化
  initlock(&bcache.lock, "bcache");
  //bucket锁的初始化
  for(int i = 0; i < NBUK; i++){
    initlock(&bcache.buckets[i].lock, "bcache.bucket");
    bcache.buckets[i].head.next = (void*)0; 
    // 先将所有的buffer放到bucket【0】中
    if (i == 0){
      prev_b = &bcache.buckets[i].head;
      for(b = bcache.buf; b < bcache.buf + NBUF; b++){
        if(b == bcache.buf + NBUF - 1) 
          b->next = (void*)0;
        prev_b->next = b;
        b->timestamp = ticks; 
        initsleeplock(&b->lock, "buffer");
        prev_b = b; 
      }    
    }
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int buk_id = hash(dev, blockno); // 哈希得到bucket的id

  //当前bucket已经有了合适的buffer
  //保证操作的原子性
  acquire(&bcache.buckets[buk_id].lock);  
  b = bcache.buckets[buk_id].head.next; // the first buf in buckets[buk_id]
  while(b){ 
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.buckets[buk_id].lock);
      acquiresleep(&b->lock);
      return b;   
    }
    b = b->next;
  }
  release(&bcache.buckets[buk_id].lock);
  
  //如果bucket中没有buffer，使用lru算法(timestamp)找出合适的buffer
  int max_timestamp = 0; 
  int lru_buk_id = -1; //
  int is_better = 0; // 
  struct buf *lru_b = (void*)0;
  struct buf *prev_lru_b = (void*)0;
  struct buf *prev_b = (void*)0;
  for(int i = 0; i < NBUK; i++){
    prev_b = &bcache.buckets[i].head;
    acquire(&bcache.buckets[i].lock);
    while(prev_b->next){
      if(prev_b->next->refcnt == 0 && prev_b->next->timestamp >= max_timestamp){ 
        max_timestamp = prev_b->next->timestamp;
        is_better = 1;
        prev_lru_b = prev_b; 
      }
      prev_b = prev_b->next;
    }
    if(is_better){
      if(lru_buk_id != -1)
        release(&bcache.buckets[lru_buk_id].lock); 
      lru_buk_id = i; 
    }
    else
      release(&bcache.buckets[i].lock); 
    is_better = 0;
  }
  lru_b = prev_lru_b->next; 
  //从通过lru算法找到的bucket中偷取buffer
  if(lru_b){
    prev_lru_b->next = prev_lru_b->next->next;
    release(&bcache.buckets[lru_buk_id].lock);
  }

  // 向目标bucket中存储buffer，保证操作的原子性
  acquire(&bcache.lock);
  acquire(&bcache.buckets[buk_id].lock);
  if(lru_b){
    lru_b->next = bcache.buckets[buk_id].head.next;
    bcache.buckets[buk_id].head.next = lru_b;
  }

  //检查该buffer是否被两个进程同时使用
  b = bcache.buckets[buk_id].head.next; 
  while(b){ 
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.buckets[buk_id].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;   
    }
    b = b->next;
  }
  //假如没找到合适的buffer
  if (lru_b == 0)
    panic("bget: no buffers");
  
  lru_b->dev = dev;
  lru_b->blockno = blockno;
  lru_b->valid = 0;
  lru_b->refcnt = 1;
  release(&bcache.buckets[buk_id].lock);
  release(&bcache.lock);
  acquiresleep(&lru_b->lock);
  return lru_b;

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
  // locker buffer的释放
  int buk_id = hash(b->dev, b->blockno);
  acquire(&bcache.buckets[buk_id].lock);
  b->refcnt--;
  // update timestamp when it is a free buf (b->refcnt == 0)
  if(b->refcnt == 0)
    b->timestamp = ticks; 
  release(&bcache.buckets[buk_id].lock);
}

void
bpin(struct buf *b) {
  int buk_id = hash(b->dev, b->blockno);
  acquire(&bcache.buckets[buk_id].lock);
  b->refcnt++;
  release(&bcache.buckets[buk_id].lock);
}

void
bunpin(struct buf *b) {
  int buk_id = hash(b->dev, b->blockno);
  acquire(&bcache.buckets[buk_id].lock);
  b->refcnt--;
  release(&bcache.buckets[buk_id].lock);
}



