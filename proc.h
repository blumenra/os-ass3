
#define MAX_PSYC_PAGES 16
#define MAX_TOTAL_PAGES 32
#define MAX_FILE_PAGES (MAX_TOTAL_PAGES - MAX_PSYC_PAGES)


// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};


enum page_state {NOT_USED, USED}; 

// pages struct
struct page_struct {
  enum page_state state;  
  pde_t* pgdir;
  uint vAddr;
  uint access_tracker;
  uint create_order;
  int adv_queue; // tracks the place in advance queue
};


enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  //Swap file. must initiate with create swap file
  struct file *swapFile;      //page file

  struct page_struct file_manager[MAX_FILE_PAGES];
  struct page_struct ram_manager[MAX_PSYC_PAGES];
  uint paged_out_count;       // counts the general number page-out occured
  uint page_fault_count;      // counts the general number of page fault times. page fault occurs when seeked page doesnt exist in the ram so we need to look for it in the file
  uint create_order_counter;  // manages the creation number for the SCFIFO policy (every new page gets a new number which represents its place in queue)
  int adv_queue_counter;      // manages the place number for the queue in AQ policy (every new page gets a new number which represents its place in queue)
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
