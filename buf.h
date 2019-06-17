/*
 * file system block
 * each buffer represent the content of one sector on a particular disk device
 * 存储在此结构中的数据通常与磁盘上的数据不同步
 */
struct buf {
  int flags; /*the flags track the relationship between memory and disk: B_VALID or B_DIRTY*/
  uint dev;  /*the dev and sector fields give the device and sector number */
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  struct buf *qnext; // disk queue
  uchar data[BSIZE]; /*block size = IDE's sector size, it's an in-memory copy of the disk sector*/
};
#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk

