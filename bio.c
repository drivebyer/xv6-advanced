// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.
//
// The implementation uses two state flags internally:
// * B_VALID: the buffer data has been read from the disk.
// * B_DIRTY: the buffer data has been modified
//     and needs to be written to disk.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.
  struct buf head;
} bcache;

/*将 bcache.buf 初始化成一个双向链表*/
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

//PAGEBREAK!
  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
/*
 * 这个函数遍历 bcache.buf 双向链表
 * 先从 bcache.head 起，正向查找链表(这里利用了程序的局部性原理)
 * 能不能找到指定参数的 block 的缓存(第一个for循环)
 * 如果找不到，再反向查找链表(第二个for循环)，找到一个b->refcnt为0
 * 且数据与磁盘同步的buffer，将这个 buffer 设置为新的参数，并返回。
 * 注意返回之前要对 buffer 加锁
 */
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached; recycle an unused buffer.
  // Even if refcnt==0, B_DIRTY indicates a buffer is in use
  // because log.c has modified it but not yet committed it.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0 && (b->flags & B_DIRTY) == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->flags = 0; /*因为需要向此buffer即将装入新的block内容，所以置零，这样在bread()中才会执行iderw()*/
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
/*
 * 指定读取一个磁盘 block，将 block 的内容放进 buf 并返回(此buffer已经被锁住)
 * 所以bread的调用者可以独占的使用返回的buf
 * 如果bread的调用者修改了buffer中的数据，那么他必须先调用bwrite将修改的数据写回磁盘
 * 然后调用brealse将锁释放
 */
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if((b->flags & B_VALID) == 0) { /*如果bget返回的buffer是一个新的，就需要去磁盘中读取数据*/
    iderw(b);
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  b->flags |= B_DIRTY; /*设置B_DIRTY，告诉iderw()做写操作，而不是读操作*/
  iderw(b);
}

// Release a locked buffer.
// Move to the head of the MRU list.
/*当buffer的使用者对buffer的操作结束后，调用brealse*/
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock); /*先释放 buffer 的锁*/

  acquire(&bcache.lock); /*由于需要对整个 bcache 遍历，加锁*/
  b->refcnt--;
  if (b->refcnt == 0) { /*说明目前已经没有用户使用这个buffer了，将buffer放在链表的最前面，表明它刚被使用过*/
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}
//PAGEBREAK!
// Blank page.

