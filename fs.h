// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint)) /* 128 */
#define MAXFILE (NDIRECT + NINDIRECT) /* 12 + 128 */

// On-disk inode structure 这个结构体占用磁盘 64 字节
struct dinode {
  short type;           // File type  dinode free:0   T_DIR:1   T_FILE:2   T_DEV:3
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          /*表明有多少个 directory entry 链接到 inode，当这个数为 0 时，type也等于0*/
  uint size;            /*inode所表示文件/目录字节数*/
  uint addrs[NDIRECT+1];   /*表明inode表示的文件内容都在哪些block中*/
};

// Inodes per block. 512/64=8
#define IPB           (BSIZE / sizeof(struct dinode)) 

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8) /*512*8 Bitmap bits per block*/

// Block of free map containing bit for block b
#define BBLOCK(b, sb) (b/BPB + sb.bmapstart) /*计算block b所在的位图的块号*/

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum; // inode number
  char name[DIRSIZ]; // DIRSIZ 14 文件名
};

