#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int sys_fork(void)
{
  return fork();
}

int sys_exit(void)
{
  exit();
  return 0; // not reached
}

int sys_wait(void)
{
  return wait();
}

int sys_kill(void)
{
  int pid;

  if (argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0)
    return -1;
  return addr;
}

int sys_sleep(void)
{
  int n;
  uint ticks0;

  if (argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (myproc()->killed)
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

/*
 * 根据homework提供的部分代码来看, data系统调用应该需要一个4字节的指针作为参数
 * 为了方便, 我们可以使用argint()来获取这个整数, 然后将其强制转换为struct rtcdate类型的指针
 * 返回 -1 说明失败, 返回 0 说明成功
 */
int sys_date(void)
{
  int temp;
  if (argint(0, &temp) < 0)
    return -1;
  cmostime((struct rtcdate *)temp);
  return 0;
}

int
sys_alarm(void)
{
  int ticks;
  void (*handler)();
  if(argint(0, &ticks) < 0)
    return -1;
  if(argptr(1, (char**)&handler, 1) < 0)
    return -1;
  /*获取到alarm()系统调用参数后，将其赋值给当前进程的成员*/
  myproc()->alarmticks = ticks;
  myproc()->alarmhandler = handler;
  return 0;
}