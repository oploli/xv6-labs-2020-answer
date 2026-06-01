#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

int
copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  struct proc *p = myproc();

  // 边界检查：不能超出进程内存，且不能回绕
  if (srcva >= p->sz || srcva + len > p->sz || srcva + len < srcva)
    return -1;

  // 直接通过内核页表中的用户映射拷贝
  // 无需遍历页表，因为内核页表已同步且无 PTE_U
  memmove(dst, (void *)srcva, len);
  return 0;
}

int
copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  struct proc *p = myproc();
  int n;

  if (srcva >= p->sz)
    return -1;

  n = 0;
  while (n < max) {
    // 不能读出进程内存边界
    if (srcva + n >= p->sz)
      return -1;
    
    char c = ((char *)srcva)[n];
    dst[n] = c;
    
    if (c == '\0')
      return 0;  // 遇到字符串结束符
    
    n++;
  }

  return -1;  // max 耗尽，没遇到 '\0'
}
