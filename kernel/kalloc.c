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

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// 跟踪每个物理页被多少个进程引用
int pg_ref_cnt[PHYSTOP / PGSIZE + 10];
struct spinlock reflock;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  // 初始化引用计数锁
  initlock(&reflock, "reflock");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    pg_ref_cnt[(uint64)p / PGSIZE] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&reflock);
  pg_ref_cnt[(uint64)pa / PGSIZE]--;
  
  // 如果还有其他进程引用这个页面,不能真正释放
  if(pg_ref_cnt[(uint64)pa / PGSIZE] > 0) {
    release(&reflock);
    return;
  }
  release(&reflock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    acquire(&reflock);
    pg_ref_cnt[(uint64)r / PGSIZE] = 1;
    release(&reflock);
  }
  return (void*)r;
}

// 增加物理页引用计数
// 在fork时共享页面调用
void
kaddref(uint64 pa)
{
  if(pa < (uint64)end || pa >= PHYSTOP)
    panic("kaddref: invalid pa");
  
  acquire(&reflock);
  pg_ref_cnt[pa / PGSIZE]++;
  release(&reflock);
}

// 获取物理页引用计数
int
kgetref(uint64 pa)
{
  int ref;
  
  if(pa < (uint64)end || pa >= PHYSTOP)
    panic("kgetref: invalid pa");
  
  acquire(&reflock);
  ref = pg_ref_cnt[pa / PGSIZE];
  release(&reflock);
  return ref;
}
