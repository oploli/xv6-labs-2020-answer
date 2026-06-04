//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

volatile int panicked = 0;

// lock to avoid interleaving concurrent printf's.
static struct {
  struct spinlock lock;
  int locking;
} pr;

static char digits[] = "0123456789abcdef";

static void
printint(int xx, int base, int sign)
{
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
printf(char *fmt, ...)
{
  va_list ap;
  int i, c, locking;
  char *s;

  locking = pr.locking;
  if(locking)
    acquire(&pr.lock);

  if (fmt == 0)
    panic("null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }
  va_end(ap);

  if(locking)
    release(&pr.lock);
}

void
panic(char *s)
{
  pr.locking = 0;
  printf("panic: ");
  printf(s);
  printf("\n");
  backtrace();
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void
printfinit(void)
{
  initlock(&pr.lock, "pr");
  pr.locking = 1;
}

// 打印函数调用栈的回溯信息
// 原理：通过帧指针遍历栈帧链
// 每个栈帧中：
// fp-8位置保存返回地址
// fp-16位置保存上一个栈帧的帧指针
void
backtrace(void)
{
  printf("backtrace:\n");
  
  // 获取当前函数的帧指针
  uint64 fp = r_fp();
  
  // 计算当前栈页的边界地址
  // xv6为每个栈分配一个页，页面对齐
  // 栈从高地址向低地址增长,故:
  // PGROUNDUP(fp)是栈页的顶部（高地址，栈的起始位置）
  // PGROUNDDOWN(fp)是栈页的底部（低地址，栈的结束位置）
  uint64 stack_top = PGROUNDUP(fp);         // 栈页上界（高地址）
  uint64 stack_bottom = PGROUNDDOWN(fp);    // 栈页下界（低地址）
  
  // 遍历栈帧链
  // 限制fp必须在当前栈页范围内
  // 当fp超出栈页范围时,说明已经到达栈底,停止遍历
  while(fp >= stack_bottom && fp < stack_top) {
    // 读取当前栈帧的返回地址=
    // *(uint64*)(fp - 8) 将fp-8转换为uint64指针,然后解引用获取地址值
    uint64 ret_addr = *(uint64*)(fp - 8);
    
    printf("%p\n", ret_addr);
    
    // 移动到上一个栈帧,读取保存在fp-16位置的上一个帧指针
    fp = *(uint64*)(fp - 16);
  }
}
