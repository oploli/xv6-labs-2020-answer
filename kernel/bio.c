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

#define NBUCKET 13

struct buf buf[NBUF];

struct bucket {
  struct spinlock lock;
  struct buf head;
} bcache[NBUCKET];

void
binit(void)
{
  struct buf *b;

  // Initialize buckets
  for(int i = 0; i < NBUCKET; i++) {
    char lock_name[16];
    snprintf(lock_name, sizeof(lock_name), "bcache_%d", i);
    initlock(&bcache[i].lock, lock_name);
    bcache[i].head.prev = &bcache[i].head;
    bcache[i].head.next = &bcache[i].head;
  }

  // Create linked list of buffers, distribute across buckets
  for(b = buf; b < buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    int bucket_id = (b - buf) % NBUCKET;
    b->next = bcache[bucket_id].head.next;
    b->prev = &bcache[bucket_id].head;
    bcache[bucket_id].head.next->prev = b;
    bcache[bucket_id].head.next = b;
  }
}

// Simple hash function for (dev, blockno)
static uint
hash(uint dev, uint blockno)
{
  return (dev + blockno) % NBUCKET;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucket_id = hash(dev, blockno);
  struct bucket *bucket = &bcache[bucket_id];

  acquire(&bucket->lock);

  // Is the block already cached?
  for(b = bucket->head.next; b != &bucket->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  // First try to find unused buffer in current bucket
  for(b = bucket->head.prev; b != &bucket->head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // No unused buffer in current bucket, steal from other buckets
  // Always scan in increasing order to avoid deadlocks
  release(&bucket->lock);
  
  for(int i = 0; i < NBUCKET; i++) {
    if(i == bucket_id)
      continue;
      
    struct bucket *other = &bcache[i];
    acquire(&other->lock);
    
    for(b = other->head.prev; b != &other->head; b = b->prev){
      if(b->refcnt == 0) {
        // Remove from old bucket
        b->next->prev = b->prev;
        b->prev->next = b->next;
        release(&other->lock);
        
        // Add to new bucket
        acquire(&bucket->lock);
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        b->next = bucket->head.next;
        b->prev = &bucket->head;
        bucket->head.next->prev = b;
        bucket->head.next = b;
        release(&bucket->lock);
        
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&other->lock);
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

  uint bucket_id = hash(b->dev, b->blockno);
  struct bucket *bucket = &bcache[bucket_id];

  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    bucket->head.next->prev = b;
    bucket->head.next = b;
  }
  
  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  uint bucket_id = hash(b->dev, b->blockno);
  acquire(&bcache[bucket_id].lock);
  b->refcnt++;
  release(&bcache[bucket_id].lock);
}

void
bunpin(struct buf *b) {
  uint bucket_id = hash(b->dev, b->blockno);
  acquire(&bcache[bucket_id].lock);
  b->refcnt--;
  release(&bcache[bucket_id].lock);
}
