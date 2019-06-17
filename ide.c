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
   * Idewait() polls the status bits r until the busy bit(IDE_BSY) is clear and the ready bit(IDE_DRDY) is set
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
 * Idestart() issues either a read or a write for the buffer's device and sector according to the flags
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
    outsl(0x1f0, b->data, BSIZE/4); /*move data to a buffer in the disk controller using outsl*/
  } else {
    outb(0x1f7, read_cmd);
  }
}

// Interrupt handler.
/*
 * 处理来自磁盘的中断
 * ideintr() consults the first buffer in the queue to find out which operation was happening
 * */
void
ideintr(void)
{
  struct buf *b;

  // First queued buffer is the active request.
  acquire(&idelock);

  if((b = idequeue) == 0){
    release(&idelock);
    return;
  }
  idequeue = b->qnext;

  // Read data if needed.
  /*
   * if the buffer was being read and the disk controller has data waiting,
   * ideintr() reads the data from a buffer in the disk controller into memory with insl instruction
   * */
  if(!(b->flags & B_DIRTY) && idewait(1) >= 0)
    insl(0x1f0, b->data, BSIZE/4);

  // Wake process waiting for this buf.
  b->flags |= B_VALID; /*set B_VALID*/
  b->flags &= ~B_DIRTY; /*clear B_DIRTY*/
  wakeup(b); /*wake up any processes sleeping on the buffer*/

  // Start disk on next buf in queue.
  /*pass the next waiting buffer to the disk*/
  if(idequeue != 0)
    idestart(idequeue);

  release(&idelock);
}

//PAGEBREAK!
// Sync buf with disk.
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
/*
 * keeeping the list of pengding disk requests in a queue 
 * and using interrupt to find out when each requests has finished
 * 
 * although iderw() maintains a queue of requests, 
 * the simple IDE disk controller can only handle one operation at a time
 */
void
iderw(struct buf *b)
{
  struct buf **pp;

  if(!holdingsleep(&b->lock))
    panic("iderw: buf not locked");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if(b->dev != 0 && !havedisk1)
    panic("iderw: ide disk 1 not present");

  acquire(&idelock);  //DOC:acquire-lock

  // Append b to idequeue.
  b->qnext = 0;
  for(pp=&idequeue; *pp; pp=&(*pp)->qnext)  //DOC:insert-queue
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
   * polling does not make efficient use of the CPU.
   * Instead,iderw() yields the CPU for other processes by sleeping,
   * waiting for the interrupt handler to record in the buffer's flag that the operation is done
   */
  while((b->flags & (B_VALID|B_DIRTY)) != B_VALID){
    sleep(b, &idelock);
  }

  release(&idelock);
}
