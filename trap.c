#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;
int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);

/*
 * set up the 256 entries in the IDT
 * tvinit: trap vector init
 */
void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  /*
   * 第二个参数为1，说明该entry为一个trap gate
   * trap gate don not clear IF flag, allowing other interrupt during the system call handler
   * DLP_USER: allow a user program to generate the trap with an explicit int instruction
   * xv6 不允许用户程序通过int指令产生其他中断（例如 divice interrupt），用户程序只能使用int 64
   * 如果使用了，就会产生保护异常，which goes to vector 13
   * 说明在执行系统调用的时候允许被其他中断打断
   */
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
/*
 * system call, device interrupt and faults 等都会从trapasm.S进入这个函数
 */
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall(); /*此时系统调用的返回值存储在tf->%eax中*/
    if(myproc()->killed)
      exit();
    return;
  }

  /*处理Page Fault的代码添加在这里*/
  //if (tf->trapno == T_PGFLT) /*只有Page Fault才处理*/
  //{
  //  pde_t *pde;
  //  pte_t *pgtab;
  //  pde_t *pte;
  //  char *mem;
  //  struct proc *curproc = myproc();
  //  uint va;
  //  /*得到造成Page Fault的虚拟地址，说明这个虚拟地址没有映射到的物理地址*/
  //  va = PGROUNDDOWN(rcr2()); /*rounddown到虚拟地址页边界*/
  //  cprintf("sz: %x\n", curproc->sz);
  //  cprintf("rcr2  va: %x\n", rcr2());
  //  cprintf("round va: %x\n", va);
  //  /*分配一页物理内存*/
  //  mem = kalloc();
  //  memset(mem, 0, PGSIZE);
  //  cprintf("pa1: %x\n", V2P(mem));
  //  pde = &((curproc->pgdir)[PDX(rcr2())]);
  //  pgtab = P2V(PTE_ADDR(*pde));
  //  pte = &pgtab[PTX(rcr2())];
  //  cprintf("Before Map PDE: %x\n", (curproc->pgdir[PDX(rcr2())]));
  //  cprintf("Before Map PTE: %x\n", *pte );
  //  cprintf("Before Map cal PA: %x\n",  PTE_ADDR((*pte)) + (rcr2() & 0b111111111111) );
  //  /*建立PTE完成映射*/
  //  memset(mem, 0, PGSIZE);
  //  mappages(curproc->pgdir, (char *)va, PGSIZE, V2P(mem), PTE_W|PTE_U);  
  //  pde = &((curproc->pgdir)[PDX(rcr2())]);
  //  pgtab = P2V(PTE_ADDR(*pde));
  //  pte = &pgtab[PTX(rcr2())];
  //  cprintf("After Map PDE: %x\n", (curproc->pgdir[PDX(rcr2())]));
  //  cprintf("After Map PTE: %x\n",  *pte );
  //  cprintf("After Map cal PA: %x\n",  PTE_ADDR((*pte)) + (rcr2() & 0b111111111111) );
  //  return;
  //}

  switch(tf->trapno){

  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    /*只处理在用户态时接受到的timer中断*/
    if (myproc() != 0 && (tf->cs & 3) == 3)
    {
      myproc()->tick_counts++;
      if (myproc()->tick_counts == myproc()->alarmticks)
      {
        /*首先需要保存现在trapframe中的eip值，也就是在for循环中停止(陷入)的地方*/
        tf->esp -= 4;
        *((uint *)(tf->esp)) = tf->eip;
        /*
         * 因为这里执行完后就要返回，并restore trapframe里的值到寄存器，恢复到用户态了，
         * 所以将trapframe里的eip设置为alarmhandler函数的值，使一返回到用户态就执行alarmhandler里保存的函数
         */ 
        tf->eip = (uint)myproc()->alarmhandler; 
        myproc()->tick_counts = 0;
      }      
    }
    
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr(); /*处理来自磁盘的中断*/
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
