# Lab5: Lazy Allocation(惰性内存分配)实验报告

> 基于 xv6-labs-2023 仓库中的 `lazy` 分支(来源 2020 课程实验),在 `192.168.131.128` 上完成。
> 本实验把进程堆内存的分配改为“惰性”方式:`sbrk()` 只增大地址空间上界 `p->sz`,**不真正分配物理内存**;当进程首次访问该地址触发缺页时,再由缺页处理程序按需分配一页物理内存并建立映射。

**测试结果:`lazytests` 与 `usertests` 均 `ALL TESTS PASSED`。**

涉及文件(共 6 个,`+106/-9` 行):
`kernel/riscv.h`、`kernel/start.c`、`kernel/sysproc.c`、`kernel/trap.c`、`kernel/vm.c`、`kernel/defs.h`。

---

## 一、详细修改比对(按实现顺序)

### 第1步,在 `kernel/riscv.h` 新增 `w_pmpcfg0()` / `w_pmpaddr0()` 内联函数

目的为:提供写 PMP(物理内存保护)CSR 的接口,以适配 qemu ≥ 7.x 对“S 态访问物理内存必须配置 PMP”的要求。

修改前(`w_satp` 之后直接是 `r_satp`,无任何 PMP 函数):

```c
static inline void 
w_satp(uint64 x)
{
  asm volatile("csrw satp, %0" : : "r" (x));
}

static inline uint64
r_satp()
```

修改后(在 `w_satp` 与 `r_satp` 之间插入两个 PMP 写函数):

```c
static inline void 
w_satp(uint64 x)
{
  asm volatile("csrw satp, %0" : : "r" (x));
}

// Physical Memory Protection (required by qemu >= 7.x so that
// supervisor mode is allowed to access all of physical memory).
static inline void
w_pmpcfg0(uint64 x)
{
  asm volatile("csrw pmpcfg0, %0" : : "r" (x));
}

static inline void
w_pmpaddr0(uint64 x)
{
  asm volatile("csrw pmpaddr0, %0" : : "r" (x));
}

static inline uint64
r_satp()
```

---

### 第2步,在 `kernel/start.c` 修改 `start()` 函数(配置 PMP)

目的为:在 `mret` 进入 S 态之前配置 PMP,让 S 态可访问全部物理内存。**这是让 2020 版 xv6 能在该环境的 qemu 7.2 上正常启动的关键修复**——否则内核在 `mret` 处陷入非法指令死循环,完全不打印任何输出。

修改前(`w_sie` 之后直接 `timerinit()`):

```c
  // delegate all interrupts and exceptions to supervisor mode.
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // ask for clock interrupts.
  timerinit();
```

修改后(在 `w_sie` 与 `timerinit()` 之间配置 PMP):

```c
  // delegate all interrupts and exceptions to supervisor mode.
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory (required by qemu >= 7.x).
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // ask for clock interrupts.
  timerinit();
```

---

### 第3步,在 `kernel/sysproc.c` 修改 `sys_sbrk()` 函数(惰性增长)

目的为:实现惰性增长——`sbrk()` 不再调用 `growproc()/uvmalloc()` 立即分配,而是只更新 `p->sz` 记录新边界;并把返回值 `addr` 改为 `uint64`(防地址超 2GB 截断,`oom` 测试需要)、`n<0` 时调 `uvmdealloc()` 释放、新边界溢出或 `>=MAXVA` 时返回 `-1`(让 `malloc` 能在地址空间耗尽时优雅返回 0)。

修改前:

```c
uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}
```

修改后:

```c
uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;

  struct proc *p = myproc();
  addr = p->sz;

  // lab5 (lazy allocation): grow the address space lazily.
  // Just record the new size; do NOT allocate physical memory yet.
  // Pages are allocated on demand by the page-fault handler when the
  // process first touches them.
  uint64 newsz;
  if(n >= 0)
    newsz = addr + (uint64)n;
  else
    newsz = addr - (uint64)(-n);

  if(n >= 0 && newsz < addr)   // wrapped past the top of the address space
    return -1;
  if(n < 0 && newsz > addr)    // would shrink below 0
    return -1;
  if(newsz >= MAXVA)           // beyond the maximum user virtual address
    return -1;

  // For a shrink, actually free the physical pages that are released.
  if(n < 0)
    p->sz = uvmdealloc(p->pagetable, addr, newsz);
  else
    p->sz = newsz;
  return addr;
}
```

---

### 第4步,在 `kernel/vm.c` 新增 `lazyalloc()` 函数

目的为:封装惰性分配的核心逻辑——仅当页表属于当前进程且出错地址落在惰性区 `[0, p->sz)` 内时,`kalloc()` 一页、清零、以 `PTE_W|PTE_R|PTE_X|PTE_U` 映射到 `PGROUNDDOWN(va)`。**关键防护**:分配前先 `walk` 检查目标页是否已存在(`PTE_V`),已存在则返回 `-1` 不映射——栈 guard page(`PTE_V=1, PTE_U=0`)或对只读页写入引发的缺页是“真正的错误”,应让调用方杀进程,而不是在 `mappages()` 里触发 `panic: remap`(这正是 `stacktest` 崩溃的根因)。

修改前:无此函数。

修改后(插在 `walkaddr()` 之前):

```c
// lab5 (lazy allocation): allocate a physical page for `va` on demand.
// Only acts on the current process's own page table, and only for
// addresses inside its lazily-grown region [0, p->sz). Returns 0 on
// success (page now mapped), -1 otherwise.
int
lazyalloc(pagetable_t pagetable, uint64 va)
{
  struct proc *p = myproc();
  if(p == 0 || pagetable != p->pagetable)
    return -1;
  if(va >= p->sz || va >= MAXVA)
    return -1;

  uint64 va0 = PGROUNDDOWN(va);

  // If the page is already present (e.g. the stack guard page, or a
  // permission fault on a read-only page), this is a genuine fault and
  // NOT a lazy miss: refuse so the caller kills the process instead of
  // panicking in mappages() with "remap".
  pte_t *pte = walk(pagetable, va0, 0);
  if(pte != 0 && (*pte & PTE_V))
    return -1;

  char *mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);

  if(mappages(pagetable, va0, PGSIZE, (uint64)mem,
              PTE_W | PTE_R | PTE_X | PTE_U) != 0){
    kfree(mem);
    return -1;
  }
  return 0;
}
```

---

### 第5步,在 `kernel/trap.c` 修改 `usertrap()` 函数(处理缺页)

目的为:捕获 13(读)/15(写)号缺页异常,从 `stval` 读取出错地址,调 `lazyalloc()` 按需分配;失败(越界或物理内存不足)则置 `p->killed=1` 杀进程。

修改前(`devintr` 之后直接是兜底的 `else`):

```c
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }
```

修改后(在两者之间插入缺页处理分支):

```c
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if(r_scause() == 13 || r_scause() == 15){
    // lab5 (lazy): load (13) or store/AMO (15) page fault.
    // The faulting virtual address is in stval; lazily allocate a
    // physical page for it. If that fails, kill the process.
    uint64 va = r_stval();
    if(lazyalloc(p->pagetable, va) < 0)
      p->killed = 1;
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }
```

---

### 第6步,在 `kernel/vm.c` 修改 `walkaddr()` 函数(内核态也能惰性分配)

目的为:让运行在内核态、不会触发用户态缺页 trap 的 `copyin/copyout/copyinstr` 在遇到未映射页时,也能调 `lazyalloc()` 按需分配并重新查找。这样 `write()`、`read()`、`exec()` 等系统调用就能访问惰性堆内存(`sbrkarg`、`rwsbrk` 等测试需要)。

修改前(页未映射直接返回 0):

```c
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
```

修改后(页未映射时尝试惰性分配并重新查找):

```c
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0){
    // lab5 (lazy): the page is not present yet. If this address belongs
    // to the current process's lazy region, allocate it now and re-walk.
    // This lets copyin/copyout/copyinstr (which run in kernel mode and
    // therefore never take a user page-fault trap) see lazy pages.
    if(lazyalloc(pagetable, va) < 0)
      return 0;
    pte = walk(pagetable, va, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      return 0;
  }
  if((*pte & PTE_U) == 0)
    return 0;
```

---

### 第7步,在 `kernel/vm.c` 修改 `uvmunmap()` 函数(不再 panic)

目的为:把“页表项不存在”与“页未映射”两处 `panic` 改为 `continue`。进程退出时 `uvmfree()` 会遍历整个 `[0, p->sz)` 区间,其中大量惰性增长但从未触碰的页并无映射,不改就会 panic。

修改前:

```c
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
```

修改后:

```c
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;   // lab5 (lazy): page-table entry not allocated
    if((*pte & PTE_V) == 0)
      continue;   // lab5 (lazy): page not present (lazy), skip it
```

---

### 第8步,在 `kernel/vm.c` 修改 `uvmcopy()` 函数(不再 panic)

目的为:同理把两处 `panic` 改为 `continue`,使 `fork()` 能复制含有大量未映射惰性页的地址空间(`sparse_memory_unmap`、`sbrkmuch` 等测试会触发)。

修改前:

```c
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
```

修改后:

```c
    if((pte = walk(old, i, 0)) == 0)
      continue;   // lab5 (lazy): page-table entry not allocated
    if((*pte & PTE_V) == 0)
      continue;   // lab5 (lazy): page not present (lazy), skip it
```

---

### 第9步,在 `kernel/vm.c` 顶部新增头文件包含

目的为:让 `lazyalloc()` / `walkaddr()` 能使用 `myproc()`、`struct proc` 及其 `sz`、`pagetable` 字段(`proc.h` 内部用到 `struct spinlock`,故需在它之前包含 `spinlock.h`)。

修改前:

```c
#include "riscv.h"
#include "defs.h"
#include "fs.h"
```

修改后:

```c
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"   // lab5 (lazy): for myproc() in lazyalloc()/walkaddr()
```

---

### 第10步,在 `kernel/defs.h` 新增 `lazyalloc()` 声明

目的为:让 `trap.c` 中的 `usertrap()` 能调用 `vm.c` 里的 `lazyalloc()`。

修改前:

```c
uint64          walkaddr(pagetable_t, uint64);
```

修改后:

```c
uint64          walkaddr(pagetable_t, uint64);
int             lazyalloc(pagetable_t, uint64);   // lab5
```

---

## 二、各修改点对应解决的测试

| 修改 | 主要解决的测试 / 问题 |
|------|----------------------|
| sys_sbrk 惰性 + MAXVA/uint64 | `lazy alloc`、`out of memory`、`sbrkbasic`(TOOMUCH) |
| usertrap 缺页处理 | `lazy alloc`、`lazy unmap`、`sbrkbasic` |
| lazyalloc(含已映射页防护)+ walkaddr | `lazy alloc`、`sbrkarg`、`stacktest` |
| uvmunmap 不 panic | 进程退出(`uvmfree`)不崩 |
| uvmcopy 不 panic | `fork()` 含惰性页不崩(`lazy unmap`、`sbrkmuch`) |
| start.c / riscv.h 配置 PMP | qemu 7.2 下能启动内核 |

## 三、测试结果

`lazytests`:

```
lazytests starting
running test lazy alloc
test lazy alloc: OK
running test lazy unmap
test lazy unmap: OK
running test out of memory
test out of memory: OK
ALL TESTS PASSED
```

`usertests`: **ALL TESTS PASSED**(50+ 项全部通过,含 `sbrkbasic`、`sbrkmuch`、`sbrkarg`、`rwsbrk`、`sbrkfail`、`stacktest`、`bsstest`、`kernmem` 等)。

> 说明:运行 `usertests` 时 `sbrkbugs` 会打印两行 `usertrap(): unexpected scause 0x000000000000000c`(12 = 指令页错误),这是该测试**故意**制造的非法跳转、期望子进程被杀死的行为,测试本身结果为 `OK`,属正常现象,不是缺陷。

## 四、核心思路说明

惰性分配的核心是“记账与分配分离”:

1. `sbrk()` 只做“记账”——抬高 `p->sz`,告诉进程“你最多可以用到这么高”,但不消耗物理内存。
2. 真正的分配推迟到“不可避免”的那一刻——进程实际读/写该地址触发 13/15 号缺页,`usertrap` 译出出错地址,分配一页并映射。
3. 因为分配被推迟,原本“假设 `[0, sz)` 全部已映射”的代码(`uvmunmap`、`uvmcopy`)会遇到大量空洞,必须由 panic 改为跳过;内核态访存的 `walkaddr` 也必须能按需补上缺失的页。
4. 容错:地址越界、`kalloc` 失败、对已映射页(只读/guard page)的违规访问,一律不再 panic,而是让进程被杀死(用户态)或让系统调用失败返回 -1(内核态),从而通过 `oom`、`sbrkbasic(TOOMUCH)`、`stacktest` 等边界测试。
