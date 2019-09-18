struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe;
  struct inode *ip;
  uint off;
};


// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            /*表明有多少个 C 指针指向这个inode，只有这个数不为零时，inode才存在于内存中，iget()和iput()修改它
                       *这个指针来自 file descriptor，current working directory，或者exec()函数
                       */
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?
  
  // copy of disk inode
  short type;         // File type  dinode free:0   T_DIR:1   T_FILE:2   T_DEV:3
  short major;
  short minor;
  short nlink;        /*表明有多少个 directory entry 链接到 inode，当这个数为 0 时，type也等于0*/
  uint size;          /*表示被inode表示对文件或者目录的内容，所占data block的大小*/
  uint addrs[NDIRECT+1];
};

// table mapping major device number to
// device functions
struct devsw {
  int (*read)(struct inode*, char*, int);
  int (*write)(struct inode*, char*, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
