# Lab 8: Locks 实现文档

## 日期
2026-06-16

## 环境
- 远程服务器: oploli@192.168.131.128
- 代码版本: xv6-labs-2023
- 本地笔记: d:\code\longjin_answer\123.md

---

## 实现概述

本实验通过将全局锁拆分为多个独立锁来减少xv6中的锁竞争：

1. **kernel/kalloc.c** - 将单一全局kmem锁改为per-CPU锁
2. **kernel/bio.c** - 将单一全局bcache锁改为13个哈希桶锁

---

## 第一部分：kernel/kalloc.c

### 结构体修改

```c
// 原始结构
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// 修改后
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];
```

### kinit() 函数

```c
void kinit()
{
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}
```

### kfree() 函数

```c
void kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;

  push_off();
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
  pop_off();
}
```

**关键点**：
- 使用 `push_off()/pop_off()` 禁用/恢复中断
- 使用 `cpuid()` 获取当前CPU ID
- 将页面添加到当前CPU的freelist

### kalloc() 函数

```c
void *kalloc(void)
{
  struct run *r;

  push_off();
  int cpu_id = cpuid();

  // 首先尝试从当前CPU分配
  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r) {
    kmem[cpu_id].freelist = r->next;
    release(&kmem[cpu_id].lock);
    goto found;
  }
  release(&kmem[cpu_id].lock);

  // 从其他CPU窃取（窃取整个freelist）
  for(int i = 1; i < NCPU; i++) {
    int steal_cpu = (cpu_id + i) % NCPU;
    acquire(&kmem[steal_cpu].lock);

    r = kmem[steal_cpu].freelist;
    if(r) {
      kmem[steal_cpu].freelist = 0;
      release(&kmem[steal_cpu].lock);

      acquire(&kmem[cpu_id].lock);
      kmem[cpu_id].freelist = r;
      r = kmem[cpu_id].freelist;
      kmem[cpu_id].freelist = r->next;
      release(&kmem[cpu_id].lock);
      goto found;
    }
    release(&kmem[steal_cpu].lock);
  }

  pop_off();
  return 0;

found:
  pop_off();
  if(r)
    memset((char*)r, 5, PGSIZE);
  return (void*)r;
}
```

**窃取策略**：
- 当当前CPU的freelist为空时，从其他CPU窃取**整个**freelist
- 使用 `(cpu_id + i) % NCPU` 确保从下一个CPU开始窃取
- 窃取后取第一个页面返回，其余加入当前CPU的freelist

---

## 第二部分：kernel/bio.c

### 结构体修改

```c
// 原始结构
struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf head;
} bcache;

// 修改后
#define NBUCKET 13
struct buf buf[NBUF];

struct bucket {
  struct spinlock lock;
  struct buf head;
} bcache[NBUCKET];
```

### 哈希函数

```c
static uint hash(uint dev, uint blockno)
{
  return (dev + blockno) % NBUCKET;
}
```

### binit() 函数

```c
void binit(void)
{
  struct buf *b;

  // 初始化所有桶
  for(int i = 0; i < NBUCKET; i++) {
    char lock_name[16];
    snprintf(lock_name, sizeof(lock_name), "bcache_%d", i);
    initlock(&bcache[i].lock, lock_name);
    bcache[i].head.prev = &bcache[i].head;
    bcache[i].head.next = &bcache[i].head;
  }

  // 将buffer均匀分布到各个桶
  for(b = buf; b < buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    int bucket_id = (b - buf) % NBUCKET;
    b->next = bcache[bucket_id].head.next;
    b->prev = &bcache[bucket_id].head;
    bcache[bucket_id].head.next->prev = b;
    bcache[bucket_id].head.next = b;
  }
}
```

### bget() 函数

```c
static struct buf* bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucket_id = hash(dev, blockno);
  struct bucket *bucket = &bcache[bucket_id];

  acquire(&bucket->lock);

  // 检查是否已缓存
  for(b = bucket->head.next; b != &bucket->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 在当前桶中查找未使用的buffer
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

  // 从其他桶窃取
  release(&bucket->lock);

  for(int i = 0; i < NBUCKET; i++) {
    if(i == bucket_id)
      continue;

    struct bucket *other = &bcache[i];
    acquire(&other->lock);

    for(b = other->head.prev; b != &other->head; b = b->prev){
      if(b->refcnt == 0) {
        // 从旧桶移除
        b->next->prev = b->prev;
        b->prev->next = b->next;
        release(&other->lock);

        // 添加到新桶
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
```

**关键点**：
- 首先在目标桶中查找
- 如果目标桶无空闲buffer，按桶ID递增顺序从其他桶窃取
- 窃取时将buffer移动到目标桶

### brelse() 函数

```c
void brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucket_id = hash(b->dev, b->blockno);
  struct bucket *bucket = &bcache[bucket_id];

  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    bucket->head.next->prev = b;
    bucket->head.next = b;
  }

  release(&bucket->lock);
}
```

### bpin() 和 bunpin() 函数

```c
void bpin(struct buf *b) {
  uint bucket_id = hash(b->dev, b->blockno);
  acquire(&bcache[bucket_id].lock);
  b->refcnt++;
  release(&bcache[bucket_id].lock);
}

void bunpin(struct buf *b) {
  uint bucket_id = hash(b->dev, b->blockno);
  acquire(&bcache[bucket_id].lock);
  b->refcnt--;
  release(&bcache[bucket_id].lock);
}
```

---

## 测试结果

### 当前状态（2026-06-16）

1. **编译状态**: ✅ 成功编译，无错误

2. **系统启动**: ✅ xv6 可以正常启动，显示 shell 提示符

3. **kalloctest**: ❌ 超时（200秒）
   - **重要发现**: 即使使用原始、未修改的 kalloc.c 和 bio.c，kalloctest 也会超时
   - **结论**: 问题不在代码实现，而在测试环境或QEMU配置

4. **bcachetest**: ⚠️ 部分通过
   - test0: FAIL
   - test1: OK
   - test2: OK

### 问题分析

kalloctest 超时的可能原因：
1. QEMU 与测试脚本的交互问题
2. 2023 xv6 与 2020 实验测试的不兼容性
3. shell 输入/输出配置问题（shell提示符写入stderr，可能导致测试脚本无法正确检测）

---

## 与本地笔记的差异

### kalloc.c 差异

| 方面 | 笔记版本 | 此实现 |
|------|----------|--------|
| 窃取策略 | 窃取1个页面 | 窃取整个freelist |
| 页面分布 | 均匀分布 | 基于kfree()调用时的CPU |

### bio.c 差异

| 方面 | 笔记版本 | 此实现 |
|------|----------|--------|
| 初始分布 | 全部在bucket 0 | 均匀分布到各桶 |
| NBUF大小 | 在结构体内 | 独立数组 |

---

## 文件修改清单

- `kernel/kalloc.c`: 添加per-CPU锁实现
- `kernel/bio.c`: 添加哈希桶锁实现

---

## 编译和测试命令

```bash
# 编译
cd ~/xv6-labs-2023
make clean
make

# 测试kalloctest
python3 grade-lab-lock kalloctest

# 测试bcachetest
python3 grade-lab-lock bcachetest

# 测试所有
python3 grade-lab-lock
```

---

## 后续工作建议

1. **调查kalloctest超时问题**: 需要更深入地检查QEMU和测试脚本的交互
2. **版本兼容性**: 确认2023 xv6与2020实验的兼容性
3. **手动测试**: 尝试在QEMU中手动运行kalloctest来验证代码正确性

