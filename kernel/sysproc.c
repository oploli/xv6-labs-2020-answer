#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  uint64 base, mask;
  int len;
  uint64 bitmask = 0;
  struct proc *p = myproc();
  pte_t *pte;

  // 直接调用，不检查返回值
  argaddr(0, &base);
  argint(1, &len);
  argaddr(2, &mask);

  if(len > 64)
    return -1;

  for(int i = 0; i < len; i++){
    uint64 va = base + i * PGSIZE;
    
    if(va >= p->sz)
      return -1;

    pte = walk(p->pagetable, va, 0);
    if(pte == 0)
      return -1;

    if(*pte & PTE_A){
      bitmask |= (1L << i);
      *pte &= ~PTE_A;
    }
  }

  if(copyout(p->pagetable, mask, (char *)&bitmask, sizeof(bitmask)) < 0)
    return -1;

  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


