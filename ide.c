// Simple PIO-based (non-DMA) IDE driver code.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

#define SECTOR_SIZE   512
#define IDE_BSY       0x80
#define IDE_DRDY      0x40
#define IDE_DF        0x20
#define IDE_ERR       0x01

#define IDE_CMD_READ  0x20
#define IDE_CMD_WRITE 0x30
#define IDE_CMD_RDMUL 0xc4
#define IDE_CMD_WRMUL 0xc5

// idequeue points to the buf now being read/written to the disk.
// idequeue->qnext points to the next buf to be processed.
// You must hold idelock while manipulating queue.

static struct spinlock idelock;
static struct buf *idequeue;

static int havedisk1;
static void idestart(struct buf*);

// Wait for IDE disk to become ready.
static int
idewait(int checkerr)
{
  int r;
  /*
   * A PC motherboard presents the status bits r of the disk hardware on I/O port 0x1f7
   * Idewait() polls the status bits r until the busy bit(IDE_BSY) is clear and 
   * the ready bit(IDE_DRDY) is set
   */
  while(((r = inb(0x1f7)) & (IDE_BSY|IDE_DRDY)) != IDE_DRDY)
    ;
  if(checkerr && (r & (IDE_DF|IDE_ERR)) != 0)
    return -1;
  return 0;
}

void
ideinit(void)
{
  int i;

  initlock(&idelock, "ide");
  /*
   * enable the IDE_IRQ interrupt only on the last CPU
   * on a two-processor system, CPU 1 handles disk interrupts
   */
  ioapicenable(IRQ_IDE, ncpu - 1);
  idewait(0); /*wait for the disk to be able to accept commands*/

  // Check if disk 1 is present
  /* 
   * probe how many disk are present
   * it assume that disk 0 is present, becouse the bootloader and kernel ware both loaded from disk 0
   * write to I/O port 0x1f6 to select disk 1, and waits a while for the status bit to show that
   * disk is ready. If not, ideinit() assumes the disk is absent
   */
  outb(0x1f6, 0xe0 | (1<<4));
  for(i=0; i<1000; i++){
    if(inb(0x1f7) != 0){
      havedisk1 = 1;
      break;
    }
  }

  // Switch back to disk 0.
  outb(0x1f6, 0xe0 | (0<<4));
}

// Start the request for b.  Caller must hold idelock.
/*
 * 这个函数执行读或者写取决于b->flags
 */
static void
idestart(struct buf *b)
{
  if(b == 0)
    panic("idestart");
  if(b->blockno >= FSSIZE)
    panic("incorrect blockno");
  int sector_per_block =  BSIZE/SECTOR_SIZE;
  int sector = b->blockno * sector_per_block;
  int read_cmd = (sector_per_block == 1) ? IDE_CMD_READ :  IDE_CMD_RDMUL;
  int write_cmd = (sector_per_block == 1) ? IDE_CMD_WRITE : IDE_CMD_WRMUL;

  if (sector_per_block > 7) panic("idestart");

  idewait(0);
  outb(0x3f6, 0);  // generate interrupt
  outb(0x1f2, sector_per_block);  // number of sectors
  outb(0x1f3, sector & 0xff);
  outb(0x1f4, (sector >> 8) & 0xff);
  outb(0x1f5, (sector >> 16) & 0xff);
  outb(0x1f6, 0xe0 | ((b->dev&1)<<4) | ((sector>>24)&0x0f));
  if(b->flags & B_DIRTY){
    outb(0x1f7, write_cmd);
    outsl(0x1f0, b->data, BSIZE/4); /*将buffer中的数据写入磁盘*/
  } else {
    outb(0x1f7, read_cmd); /*发起一个读操作，磁盘控制器有数据后，产生一个中断，让ideintr()去处理*/
  }
}

// Interrupt handler.
/*
 * 处理来自磁盘的中断，对应队列中第一个buffer
 * 要么是磁盘控制器上有新的数据，需要读取
 * 要么是告诉 OS 之前的写操作已经完成
 * 这取决于前面 iderw()-> idestart() 执行的写操作还是读操作
 */
void
ideintr(void)
{
  struct buf *b;

  // First queued buffer is the active request.
  acquire(&idelock);

  if((b = idequeue) == 0){
    release(&idelock); /*队列中的buffer全部处理完毕，直接返回*/
    return;
  }
  idequeue = b->qnext; /*将链头指针后移，处理第一个buffer*/

  // Read data if needed.
  /*
   * 磁盘控制器里有数据，读取到buffer中
   */
  if(!(b->flags & B_DIRTY) && idewait(1) >= 0)
    insl(0x1f0, b->data, BSIZE/4);

  // Wake process waiting for this buf.
  b->flags |= B_VALID; /*set B_VALID*/
  b->flags &= ~B_DIRTY; /*clear B_DIRTY*/
  wakeup(b); /*通知等待的其他进程，此buffer已经处理完毕*/

  // Start disk on next buf in queue.
  /*接着处理下一个队列中的 buffer*/
  if(idequeue != 0)
    idestart(idequeue);

  release(&idelock);
}

//PAGEBREAK!
// Sync buf with disk.
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
/*
 * 将对磁盘的请求放进 idequeue 队列里，然后使用中断来找到哪个请求已经完成 ideintr()
 * 虽然 iderw() 维护了一个磁盘请求的队列 idequeue，但是磁盘控制器一次也只能处理一个请求
 */
void
iderw(struct buf *b)
{
  struct buf **pp;

  if(!holdingsleep(&b->lock)) /*磁盘操作前必须先拿到这个 buf 的锁*/
    panic("iderw: buf not locked");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID) /*B_DIRTY = 0，即数据没有脏，不需要写回磁盘*/
    panic("iderw: nothing to do");
  if(b->dev != 0 && !havedisk1)
    panic("iderw: ide disk 1 not present");

  acquire(&idelock);  //DOC:acquire-lock

  // Append b to idequeue.
  b->qnext = 0;
  for(pp=&idequeue; *pp; pp=&(*pp)->qnext) /*二级指针遍历链表*/
    ;
  *pp = b;

  // Start disk if necessary.
  /*
   * 即如果刚插入的buffer是队列中的第一个buffer，就立即调用idestart()将buffer送到disk
   * otherwise the buffer will be started once the buffers ahead of it are taken care of
   */
  if(idequeue == b)
    idestart(b);

  // Wait for request to finish.
  /*
   * 等待磁盘中断处理函数将 B_DIRTY 设置为 0，即该 buffer 的数据已经写入磁盘
   */
  while((b->flags & (B_VALID|B_DIRTY)) != B_VALID){
    sleep(b, &idelock);
  }

  release(&idelock);
}
