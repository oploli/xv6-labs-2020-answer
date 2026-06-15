# xv6 Thread Lab 实验报告

本报告详细说明了完成 MIT 6.S081 xv6 Thread Lab 所做的所有修改。

---

======================================================================

## 第1步，在 user/uthread_switch.S 中做以下修改

目的是实现用户级线程的上下文切换，保存和恢复寄存器状态。

### 修改位置：thread_switch 函数

### 修改内容：

```assembly
.globl thread_switch
thread_switch:
	sd	ra, 0(a0)
	sd	sp, 8(a0)
	sd	s0, 16(a0)
	sd	s1, 24(a0)
	sd	s2, 32(a0)
	sd	s3, 40(a0)
	sd	s4, 48(a0)
	sd	s5, 56(a0)
	sd	s6, 64(a0)
	sd	s7, 72(a0)
	sd	s8, 80(a0)
	sd	s9, 88(a0)
	sd	s10, 96(a0)
	sd	s11, 104(a0)

	ld	ra, 0(a1)
	ld	sp, 8(a1)
	ld	s0, 16(a1)
	ld	s1, 24(a1)
	ld	s2, 32(a1)
	ld	s3, 40(a1)
	ld	s4, 48(a1)
	ld	s5, 56(a1)
	ld	s6, 64(a1)
	ld	s7, 72(a1)
	ld	s8, 80(a1)
	ld	s9, 88(a1)
	ld	s10, 96(a1)
	ld	s11, 104(a1)

	ret
```

### 说明：

1. 前12条 `sd` 指令保存当前线程的寄存器到 a0 指向的内存位置
2. 后12条 `ld` 指令从 a1 指向的内存位置恢复新线程的寄存器
3. `ret` 指令返回到恢复的 ra 地址（即新线程的执行位置）

寄存器保存顺序与 `struct thread` 中的 context 数组布局一致：
- context[0] = ra (返回地址)
- context[1] = sp (栈指针)
- context[2-11] = s0-s11 (被调用者保存寄存器)

======================================================================

## 第2步，在 user/uthread.c 的 struct thread 结构体中做以下修改

目的是为每个线程添加保存寄存器状态的 context 数组。

### 修改位置：struct thread 结构体

### 修改前：

```c
struct thread {
  char       stack[STACK_SIZE];
  int        state;
};
```

### 修改后：

```c
struct thread {
  char       stack[STACK_SIZE];
  int        state;
  uint64     context[12];  /* 添加：保存寄存器状态 */
};
```

### 说明：

添加 context 数组用于存储12个寄存器（ra, sp, s0-s11），每个占8字节（uint64），总共96字节。

======================================================================

## 第3步，在 user/uthread.c 的 thread_schedule 函数中做以下修改

目的是在切换线程时调用 thread_switch 来保存/恢复寄存器状态。

### 修改位置：thread_schedule 函数中的 "YOUR CODE HERE" 部分

### 修改前：

```c
if (current_thread != next_thread) {
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    /* YOUR CODE HERE
     * Invoke thread_switch to switch from t to next_thread:
     * thread_switch(??, ??);
     */
}
```

### 修改后：

```c
if (current_thread != next_thread) {
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    thread_switch((uint64)&t->context, (uint64)&next_thread->context);
}
```

### 说明：

1. 第一个参数 `&t->context` 是当前线程的 context 地址，用于保存当前寄存器
2. 第二个参数 `&next_thread->context` 是新线程的 context 地址，用于恢复新线程的寄存器
3. thread_switch 返回后，CPU 将在新线程的上下文中执行

======================================================================

## 第4步，在 user/uthread.c 的 thread_create 函数中做以下修改

目的是初始化新线程的栈和寄存器状态，使其能够正确开始执行。

### 修改位置：thread_create 函数中的 "// YOUR CODE HERE" 部分

### 修改前：

```c
void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;
  // YOUR CODE HERE
}
```

### 修改后：

```c
void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;
  
  // 设置线程的栈指针到栈顶
  uint64 sp = (uint64)&t->stack[STACK_SIZE];
  t->context[1] = sp;  // sp
  
  // 设置返回地址为线程函数，thread_switch 返回时将跳转到此函数
  t->context[0] = (uint64)func;  // ra
  
  // 初始化其他寄存器为 0
  for(int i = 2; i < 12; i++) {
    t->context[i] = 0;
  }
}
```

### 说明：

1. **栈指针设置**：sp 指向栈的顶部（stack[STACK_SIZE]），因为 RISC-V 栈向下增长
2. **返回地址设置**：ra 设置为 func 函数地址，当 thread_switch 执行 ret 时会跳转到 func
3. **其他寄存器初始化**：s0-s11 初始化为 0，新线程开始时这些寄存器为 0

======================================================================

## 第5步，在 notxv6/ph.c 中添加锁数组声明

目的是为每个哈希桶添加一个独立的锁，支持并行访问。

### 修改位置：全局变量声明区域（struct entry *table[NBUCKET]; 之后）

### 修改内容：

```c
pthread_mutex_t lock[NBUCKET];  // 为每个 bucket 添加一个锁
```

### 说明：

添加 NBUCKET 个互斥锁，每个桶一个锁，这样访问不同桶的线程可以并行执行。

======================================================================

## 第6步，在 notxv6/ph.c 的 main 函数中初始化锁

目的是在程序开始时初始化所有锁。

### 修改位置：main 函数开始处

### 修改内容：

```c
int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  double t1, t0;

  // 初始化所有锁
  for(int i = 0; i < NBUCKET; i++) {
    pthread_mutex_init(&lock[i], NULL);
  }

  // ... 原有代码 ...
}
```

### 说明：

在使用锁之前必须先初始化，为每个桶的锁调用 pthread_mutex_init。

======================================================================

## 第7步，在 notxv6/ph.c 的 put 函数中添加锁保护

目的是保护哈希表的插入和更新操作，防止竞争条件。

### 修改位置：put 函数

### 修改内容：

```c
static 
void put(int key, int value)
{
  int i = key % NBUCKET;

  pthread_mutex_lock(&lock[i]);  // 获取桶锁
  
  // is the key already present?
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }
  if(e){
    // update the existing key.
    e->value = value;
  } else {
    // the new is new.
    insert(key, value, &table[i], table[i]);
  }
  
  pthread_mutex_unlock(&lock[i]);  // 释放桶锁
}
```

### 说明：

1. 在操作桶之前获取对应的锁
2. 完成操作后释放锁
3. 只锁定当前操作的桶，不影响其他桶的访问

======================================================================

## 第8步，在 notxv6/ph.c 的 get 函数中添加锁保护

目的是保护哈希表的查找操作，防止在遍历链表时数据被修改。

### 修改位置：get 函数

### 修改内容：

```c
static struct entry*
get(int key)
{
  int i = key % NBUCKET;

  pthread_mutex_lock(&lock[i]);  // 获取桶锁
  
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }
  
  pthread_mutex_unlock(&lock[i]);  // 释放桶锁
  
  return e;
}
```

### 说明：

与 put 函数类似，需要在访问桶之前获取锁，完成后释放锁。

======================================================================

## 第9步，在 notxv6/barrier.c 的 barrier 函数中实现屏障同步

目的是让所有线程在到达某个点后等待，直到所有线程都到达后才继续执行。

### 修改位置：barrier 函数中的 "// YOUR CODE HERE" 部分

### 修改前：

```c
static void 
barrier()
{
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //
}
```

### 修改后：

```c
static void 
barrier()
{
  pthread_mutex_lock(&bstate.barrier_mutex);
  
  // 增加到达屏障的线程计数
  bstate.nthread += 1;
  
  // 如果是最后一个线程到达屏障
  if(bstate.nthread == nthread) {
    // 增加轮次计数
    bstate.round += 1;
    // 重置线程计数，为下一轮做准备
    bstate.nthread = 0;
    // 唤醒所有等待的线程
    pthread_cond_broadcast(&bstate.barrier_cond);
  } else {
    // 等待直到屏障释放
    // 使用 while 循环处理虚假唤醒
    int current_round = bstate.round;
    while(bstate.round == current_round) {
      pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
    }
  }
  
  pthread_mutex_unlock(&bstate.barrier_mutex);
}
```

### 说明：

1. **获取互斥锁**：保护共享变量 bstate.nthread 和 bstate.round
2. **增加线程计数**：表示当前线程已到达屏障
3. **最后一个线程**：唤醒所有等待线程并进入下一轮
4. **其他线程**：等待条件变量，直到轮次改变
5. **使用 while 循环**：处理可能的虚假唤醒（spurious wakeup）
6. **释放互斥锁**：让其他线程可以进入

======================================================================

## 测试结果

所有测试均通过，最终得分：**60/60**

### 1. uthread 测试
- 三个线程正确创建和执行
- 线程按正确顺序轮转（c -> a -> b）
- 每个线程执行100次后正确退出
- 输出："thread_schedule: no runnable threads"

### 2. ph_safe 测试
- 多线程环境下无键丢失
- 2个线程测试结果：0 keys missing

### 3. ph_fast 测试
- 2个线程比1个线程快约1.3-1.4倍
- 满足1.25倍加速要求

### 4. barrier 测试
- 多线程正确同步
- 输出："OK; passed"

---

## 总结

本实验成功实现了：
1. **用户级线程**：通过手动保存/恢复寄存器实现线程切换
2. **细粒度锁**：为哈希表每个桶添加独立锁，消除竞争并允许并行
3. **屏障同步**：使用条件变量实现线程屏障

关键概念：
- 线程上下文切换需要保存 callee-saved 寄存器（ra, sp, s0-s11）
- 细粒度锁可以比单一锁提供更好的并行性
- 条件变量需要配合互斥锁使用，并用 while 循环处理虚假唤醒
