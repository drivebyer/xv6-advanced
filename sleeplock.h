// Long-term locks for processes
struct sleeplock {
  /*a word that is zero when the lock is avaliable and non-zero when it is held*/
  uint locked;       // Is the lock held?
  struct spinlock lk; // spinlock protecting this sleep lock
  
  // For debugging:
  char *name;        // Name of lock.
  int pid;           // Process holding lock
};

