//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}


// ============================================================
// mmap / munmap  (Lab: mmap)
// ============================================================

// Find the VMA (if any) whose range covers virtual address va.
static struct vmarea*
vma_find(struct proc *p, uint64 va)
{
  for(int i = 0; i < NVMA; i++){
    struct vmarea *v = &p->vmas[i];
    if(v->addr != 0 && va >= v->addr && va < v->addr + v->len)
      return v;
  }
  return 0;
}

// Write one mapped page back to its file (MAP_SHARED only).
// Only the bytes that fall inside the file are written, so the
// mapping never extends the file.
static void
vma_writeback(struct vmarea *v, uint64 pa, uint64 off)
{
  struct inode *ip = v->f->ip;
  uint n;

  ilock(ip);
  if(off >= ip->size){
    iunlock(ip);
    return;
  }
  n = (off + PGSIZE > ip->size) ? ip->size - off : PGSIZE;
  iunlock(ip);

  begin_op();
  ilock(ip);
  writei(ip, 0, pa, off, n);
  iunlock(ip);
  end_op();
}

// Remove the mappings for [start, start+len) (page-aligned),
// writing back dirty MAP_SHARED pages.  A VMA whose whole region
// has been unmapped has its file closed and its slot freed.
static void
vma_do_unmap(struct proc *p, uint64 start, uint64 len)
{
  uint64 s = PGROUNDDOWN(start);
  uint64 e = PGROUNDUP(start + len);

  for(int i = 0; i < NVMA; i++){
    struct vmarea *v = &p->vmas[i];
    if(v->addr == 0)
      continue;
    uint64 is = (s > v->addr) ? s : v->addr;
    uint64 ie = (e < v->addr + v->len) ? e : (v->addr + v->len);
    if(is >= ie)
      continue;                       // no overlap with this VMA

    int fully = (s <= v->addr && e >= v->addr + v->len);

    for(uint64 a = is; a < ie; a += PGSIZE){
      pte_t *pte = walk(p->pagetable, a, 0);
      if(pte == 0 || (*pte & PTE_V) == 0)
        continue;                     // page was never faulted in
      uint64 pa = PTE2PA(*pte);
      if((v->flags & MAP_SHARED) && (v->prot & PROT_WRITE))
        vma_writeback(v, pa, v->offset + (a - v->addr));
      kfree((void*)pa);
      *pte = 0;
    }

    if(fully){
      fileclose(v->f);
      v->addr = 0;
      v->f = 0;
      v->len = 0;
    }
  }
}

// Handle a page fault inside an mmap region: lazily allocate the
// page, fill it from the file, and map it.  Returns 0 on success,
// -1 if va is not within any VMA or allocation fails.
int
mmap_fault(uint64 va)
{
  struct proc *p = myproc();
  struct vmarea *v;
  char *mem;
  int perm;

  if((v = vma_find(p, va)) == 0)
    return -1;

  va = PGROUNDDOWN(va);
  if((mem = kalloc()) == 0)
    return -1;
  memset(mem, 0, PGSIZE);            // bytes past EOF must read as zero

  ilock(v->f->ip);
  readi(v->f->ip, 0, (uint64)mem, v->offset + (va - v->addr), PGSIZE);
  iunlock(v->f->ip);

  perm = PTE_U;
  if(v->prot & PROT_READ)  perm |= PTE_R;
  if(v->prot & PROT_WRITE) perm |= PTE_W;
  if(v->prot & PROT_EXEC)  perm |= PTE_X;

  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0){
    kfree(mem);
    return -1;
  }
  return 0;
}

// sys_mmap: record a (lazily realised) file mapping.  The kernel
// chooses the virtual address, growing downward from TRAPFRAME.
uint64
sys_mmap(void)
{
  uint64 addr, len, offset;
  int prot, flags, fd;
  struct file *f;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &offset);

  if(len == 0)
    return (uint64)-1;
  if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
    return (uint64)-1;
  // a shared, writable mapping requires a writable file
  if((flags & MAP_SHARED) && (prot & PROT_WRITE) && !f->writable)
    return (uint64)-1;

  struct vmarea *v = 0;
  for(int i = 0; i < NVMA; i++)
    if(p->vmas[i].addr == 0){ v = &p->vmas[i]; break; }
  if(v == 0)
    return (uint64)-1;

  uint64 alen = PGROUNDUP(len);
  uint64 base = TRAPFRAME;
  for(int i = 0; i < NVMA; i++)
    if(p->vmas[i].addr != 0 && p->vmas[i].addr < base)
      base = p->vmas[i].addr;
  uint64 va = base - alen;
  if(va <= p->sz)                     // keep clear of the heap
    return (uint64)-1;

  v->addr   = va;
  v->len    = alen;
  v->prot   = prot;
  v->flags  = flags;
  v->offset = offset;
  v->f      = filedup(f);            // survive close(fd)

  return va;
}

// sys_munmap: remove (part of) a mapping, writing back dirty pages.
uint64
sys_munmap(void)
{
  uint64 addr, len;
  argaddr(0, &addr);
  argaddr(1, &len);
  if(len == 0)
    return 0;
  vma_do_unmap(myproc(), addr, len);
  return 0;
}

// Copy a parent's VMAs into a freshly forked child, giving the
// child its own file reference for each mapping.
void
vma_fork(struct proc *parent, struct proc *child)
{
  for(int i = 0; i < NVMA; i++){
    child->vmas[i] = parent->vmas[i];
    if(child->vmas[i].addr != 0)
      child->vmas[i].f = filedup(parent->vmas[i].f);
  }
}

// Remove every mapping of a process (called from exit): write back
// dirty shared pages, free physical pages, and close the files.
void
vma_exit(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    struct vmarea *v = &p->vmas[i];
    if(v->addr == 0)
      continue;
    for(uint64 a = v->addr; a < v->addr + v->len; a += PGSIZE){
      pte_t *pte = walk(p->pagetable, a, 0);
      if(pte && (*pte & PTE_V)){
        uint64 pa = PTE2PA(*pte);
        if((v->flags & MAP_SHARED) && (v->prot & PROT_WRITE))
          vma_writeback(v, pa, v->offset + (a - v->addr));
        kfree((void*)pa);
        *pte = 0;
      }
    }
    fileclose(v->f);
    v->addr = 0;
    v->f = 0;
    v->len = 0;
  }
}
