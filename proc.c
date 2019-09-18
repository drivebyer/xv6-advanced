#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc; /* 这个全局变量在userinit()函数中赋值 */

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  /*
    初始化进程全局变量进程锁
  */
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.

/* 
 * allocproc is to allocate a slot(struct proc) in the process table
 * and to initialize the parts of the process's state required for 
 * its kernel thread to execute.
 * 
 * allocproc is called for each new process 
 *
 * allocproc is written so that it can be used by fork
 * allocproc sets up the new process with a specially prepared kernel stack 
 * and set of kernel registers that cause it to "return" to user space when it first runs
 * 
 * 与linux kernel一样，每个进程有一个相应的内核栈
 */
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  /*获取锁*/
  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  /*释放锁*/
  release(&ptable.lock);
  return 0;

  // ///////////////////
  // 在进程表里找到一个进程
  // ///////////////////

found:
  p->state = EMBRYO; /*EMBRYO:萌芽*/
  p->pid = nextpid++;
  //p->tick_counts = 0;
  //p->alarmticks = -1;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){ /*allocate a kernel stack for the process's kernel thread*/
    p->state = UNUSED;
    return 0;
  }

  // 记住sp的结束位置 
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;
  /*struct context {
    uint edi;
    uint esi;
    uint ebx;
    uint ebp;
    uint eip;
  };*/
  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);

  /*
   * 由于内核线程开始执行的时候，会将p-context的内容拷贝进寄存器
   * 所以内核线程执行的第一个函数为forkret
   */
  p->context->eip = (uint)forkret;
  /*
  +------------+ <--- old_sp
  | trapframe  | 假设这个进程是从用户态进来的，所以留有trapframe空间
  +------------+ <--- tf/new_sp1, trapframe里的值将会在上层函数userinit中设置好
  |  trapret   |
  +------------+ <--- new_sp2,the address that forkret will return to
  |eip(forkret)|
  +------------+ this setting will cause the kernel thread execute at the start of forkret
  |    ...     |
  +------------+
  |    edi     |
  +------------+ <--- new_sp3(p->context)
  |   empty    |  
  +------------+ <--- p->kstack = kalloc()

  */
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
/*
 * userinit() and setupkvm() create an whole address space
 * inituvm() 则负责完成 user part of address space
 */
void
userinit(void) /*这个函数只调用一次, 创建的init process是所有进程的父进程*/
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[]; /*这是initcode.S的加载位置和大小*/

  p = allocproc(); /*这个函数每次创建进程的时候都会调用*/
  
  initproc = p;

  // 创建该进程内核空间页表相关的内容
  if((p->pgdir = setupkvm()) == 0) /*create a page table*/
    panic("userinit: out of memory?");
  
  // 创建该进程用户空间页表相关内容
  // 这时已经决定了要载入initcode这个二进制文件了
  // 由于目前xv6不支持文件系统，所以选择将这个二进制文件直接编译进内核，
  // _binary_initcode_start和_binary_initcode_size描述的该二进制文件在内核镜像中的位置
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));

  /*这里设置的trapframe的内容后面会restore到相应的寄存器中*/
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF; /*this bit set to allow hardware interrupt*/
  
  // 因为在上面的inituvm()函数中, 暂时只分配了一页物理内存, 映射到虚拟内存那也只有一页大小, 
  // 所以暂时有效虚拟地址范围为 0x0～0x1000，即initcode二进制文件存在与这一段空间
  // 这里有点奇怪的是这里process image与用户栈相连？
  p->tf->esp = PGSIZE; 
  p->tf->eip = 0; /*entry point of initcode.S, address 0*/

  safestrcpy(p->name, "initcode", sizeof(p->name)); /*p->name mainly for debugging*/
  p->cwd = namei("/"); /*set the process's current working directory*/

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE; /*RUNNABLE state marks it available for scheduling*/

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
/*
 * If n is positive, growproc() allocates one or more physical pages 
 * and maps them at the top of the process's address space.
 * If n is negative, growproc() unmaps one or more pages from the process's
 * address space and free the corresponding physical pages.
 */
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz; /*proc->sz is the process's current size*/
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc); /*sets %*/
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  /*让子进程和父进程的trapframe相同，这样子进程回到用户空间后，才会和父进程回到的地方一样*/
  *np->tf = *curproc->tf; 
  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);

  np->cwd = idup(curproc->cwd);
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));
  pid = np->pid;
  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE) 
        continue;

      // ////////////////////////////////
      // 运行刚找到的进程状态为RUNNABLE的进程
      // ////////////////////////////////

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.

      // 将找到的进程设置为CPU当前执行的进程
      c->proc = p; 

      // 这里主要是为了切换到目标进程到页目录
      // 在内核执行的时候切换页表是可以的, 因为在setupkvm()中将所有页表的内核映射都设置相同
      switchuvm(p); /*tell the hardware to start using the target process's page table*/
      p->state = RUNNING;

      /* 
       * this perform a context switch to the target process's kernel thread
       * the current context is not a process but rather a special per-cpu scheduler context,
       * so scheduler tells swtch() to save the current hardware registers in 
       * per-cpu storage(cpu->scheduler) rather than in any process's kernel thread context
       */ 
      swtch(&(c->scheduler), p->context); 
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
/*
 * the final ret instruction(3078) in swtch(..., ...) 
 * pop the target process's %eip from the kernel stack, finishing the context switch
 * allocproc had previously set initproc's p->context->eip to forkret,
 * so the ret starts executing forkret
 */
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process with its own kernel stack(e.g., they call sleep), 
    // and thus cannot be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
