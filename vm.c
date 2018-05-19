#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// int getSCFIFO(){
//   pte_t * pte;
//   int i = 0;
//   int pageIndex;
//   uint loadOrder;

// recheck:
//   pageIndex = -1;
//   loadOrder = 0xFFFFFFFF;
//   for (i = 0; i < MAX_PYSC_PAGES; i++) {
//     if (proc->ramCtrlr[i].state == USED && proc->ramCtrlr[i].loadOrder <= loadOrder){
//       pageIndex = i;
//       loadOrder = proc->ramCtrlr[i].loadOrder;
//     }
//   }
//   pte = walkpgdir(proc->ramCtrlr[pageIndex].pgdir, (char*)proc->ramCtrlr[pageIndex].userPageVAddr,0);
//   if (*pte & PTE_A) {
//     *pte &= ~PTE_A; // turn off PTE_A flag
//      proc->ramCtrlr[pageIndex].loadOrder = proc->loadOrderCounter++;
//      goto recheck;
//   }
//   return pageIndex;
// }

/*
* Gets the index of page in memory which should be swapped-out according to the defined policy
*/
int getPageOutIndex(){
  #if NFUA
    return getNFUA();
  #endif
  #if LAPA
    return getLAPA();
  #endif
  #if SCFIFO
    return getSCFIFO();
  #endif
  #if AQ
    return getAQ();
  #endif
  panic("Unrecognized paging machanism");
}

int getNFUA(void){

  // cprintf("IM IN getNFUA!\n");
  struct proc* p = myproc();

  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ramCtrlr[i].state == NOTUSED)
      continue;
    
    if((min == -1) || (p->ramCtrlr[min].accessTracker > p->ramCtrlr[i].accessTracker)){
      min = i;
    }
  }

  // cprintf("Returing min=%d from getNFUA", min);
  return min;
}

int getLAPA(void){

  struct proc* p = myproc();

  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ramCtrlr[i].state == NOTUSED)
      continue;

    if((min == -1) || (countNumOfOneBits(p->ramCtrlr[min].accessTracker) > countNumOfOneBits(p->ramCtrlr[i].accessTracker))){
      min = i;
    }
  }

  return min;
}


int getSCFIFO(void){

  struct proc* p = myproc();
  pte_t* pte;
  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ramCtrlr[i].state == NOTUSED)
      continue;

    if((min == -1) || (p->ramCtrlr[min].loadOrder > p->ramCtrlr[i].loadOrder)){
      min = i;
    }
  }

  pte = walkpgdir(p->ramCtrlr[min].pgdir, (char*)p->ramCtrlr[min].userPageVAddr,0);
  
  // If the page was accessed..
  if (*pte & PTE_A) {
    *pte &= ~PTE_A; // turn off PTE_A flag
     p->ramCtrlr[min].loadOrder = p->loadOrderCounter++; // update page's loadOrder to be the highest (treat it like it was just created)
     return getSCFIFO();
  }

  return min;
}

int getAQ(void){

  struct proc* p = myproc();

  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ramCtrlr[i].state == NOTUSED)
      continue;

    if((min == -1) || (p->ramCtrlr[min].advQueue > p->ramCtrlr[i].advQueue)){
      min = i;
    }
  }

  return min;
}


uint countNumOfOneBits(uint n){
    uint counter = 0;
    while(n) {
        counter += n % 2;
        n >>= 1;
    }
    return counter;
}




int findNextAQPageIndex(struct proc* p, int index){
  
  
  int ans = -1;
  if(index >= 0){ // Find the next in advQueu after page
    ans = findNextAdvPageIndex(p, p->ramCtrlr[index].advQueue);
  }
  else{ // Find the first in advQueu
    ans = findMinAdvPageIndex(p);
  }

  return ans;
}

int findNextAdvPageIndex(struct proc* p, int boundery){

  uint min = -1;
  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ramCtrlr[i].state == NOTUSED)
      continue;
    if(p->ramCtrlr[i].advQueue <= boundery)
      continue;

    // Here p->ramCtrlr[i] is in the ram and grater than boundery
    if((min < 0) || (p->ramCtrlr[min].advQueue > p->ramCtrlr[i].advQueue)){
      min = i;
    }
  }

  return min;
}


int findMinAdvPageIndex(struct proc* p){
  return findNextAdvPageIndex(p, 0xFFFFFFFF);
}

void swapAdvQueue(struct proc* p, int priorPageIndex, int afterPageIndex){

  int tmpTurn = 0;
  tmpTurn = p->ramCtrlr[priorPageIndex].advQueue;
  p->ramCtrlr[priorPageIndex].advQueue = p->ramCtrlr[afterPageIndex].advQueue;
  p->ramCtrlr[afterPageIndex].advQueue = tmpTurn;
}

int isPageAccessed(struct proc* p, int index){

  pte_t* pte = walkpgdir(p->ramCtrlr[index].pgdir, (char*)(p->ramCtrlr[index].userPageVAddr), 0);
  return (*pte & PTE_A);
}

/*
* Returns the pysical address mapped to the virtual address userPageVAddr
*/
int getPagePAddr(int userPageVAddr, pde_t * pgdir){

  // Get the a pointer to the PTE of userPageVAddr in pgdir (Dont allocate new PTE if didnt found.. (third parameter))
  pte_t* pte = walkpgdir(pgdir, (int*)userPageVAddr, 0);

  if(!pte) //uninitialized page table
    return -1;

  return PTE_ADDR(*pte);
}

/*
* Change PTE flags properly after swapping-out userPageVAddr
*/
void fixPagedOutPTE(int userPageVAddr, pde_t * pgdir){
  
  struct proc* p = myproc();
  
  pte_t *pte;
  pte = walkpgdir(pgdir, (int*)userPageVAddr, 0);
  if (!pte)
    panic("PTE of swapped page is missing");
  *pte |= PTE_PG; // Inidicates that the page was Paged-out to secondary storage
  *pte &= ~PTE_P; // Indicates that the page is NOT in physical memory
  *pte &= PTE_FLAGS(*pte); //clear junk physical address
  lcr3(V2P(p->pgdir)); //refresh CR3 register (TLB (cache))
}

/*
* Checks if page corresponding to userPageVAddr is indeed in swapfile (e.g not in memory)
*/
int pageIsInFile(int userPageVAddr, pde_t * pgdir) {
  pte_t *pte;
  pte = walkpgdir(pgdir, (char *)userPageVAddr, 0);
  return (*pte & PTE_PG); //PAGE IS IN FILE
}

/*
* Finds an available room for page in memory and returns its index
*/
int getFreeRamCtrlrIndex() {
  if (myproc() == 0)
    return -1;
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    if (myproc()->ramCtrlr[i].state == NOTUSED)
      return i;
  }

  // If got here, it means that no room for pages in memory is left
  return -1;
}

/*
* Finds an available page in memory and updates its virtual address to userPageVAddr, etc.
*/
void addToRamCtrlr(pde_t *pgdir, uint userPageVAddr) {

  struct proc* p = myproc();

  int freeLocation = getFreeRamCtrlrIndex();
  p->ramCtrlr[freeLocation].state = USED;
  p->ramCtrlr[freeLocation].pgdir = pgdir;
  p->ramCtrlr[freeLocation].userPageVAddr = userPageVAddr;
  p->ramCtrlr[freeLocation].loadOrder = p->loadOrderCounter++;
  p->ramCtrlr[freeLocation].accessTracker = 0;

  p->ramCtrlr[freeLocation].advQueue = p->advQueueCounter--;
}



/*
* Swaps a sigle page between memory and file by finding the page to swap-out (according to the policy),
* finding a available place in file for the above and put it in it.
* After that, 
*
*/
void swap(pde_t *pgdir, uint userPageVAddr){
  
  struct proc *p = myproc();

  p->countOfPagedOut++;

  // Get the index of page in memory which should be swapped out according to the policy
  int outIndex = getPageOutIndex();

  // Get the physical address mapped to the virtual address p->ramCtrlr[outIndex].userPageVAddr in page directory p->ramCtrlr[outIndex].pgdir
  int outPagePAddr = getPagePAddr(p->ramCtrlr[outIndex].userPageVAddr, p->ramCtrlr[outIndex].pgdir);

  // Swap-out page starting in p->ramCtrlr[outIndex].userPageVAddr
  writePageToFile(p, p->ramCtrlr[outIndex].userPageVAddr, p->ramCtrlr[outIndex].pgdir);
  
  // Converts physical address to virtual address
  char *v = (char*)P2V(outPagePAddr);
  
  //free swapped-out page
  kfree(v);

  // Change state of swapped-out page in MEMORY to UNUSED
  p->ramCtrlr[outIndex].state = NOTUSED;

  // Fix PTE flags properly after swapping-out userPageVAddr
  fixPagedOutPTE(p->ramCtrlr[outIndex].userPageVAddr, p->ramCtrlr[outIndex].pgdir);

  // Finds an available page in memory and updates its virtual address to be userPageVAddr
  addToRamCtrlr(pgdir, userPageVAddr);
}

/*
* Updates PTE flags of userPageVAddr after swapping-in a page
*/
void fixPagedInPTE(int userPageVAddr, int pagePAddr, pde_t * pgdir){
  pte_t *pte;
  pte = walkpgdir(pgdir, (int*)userPageVAddr, 0);
  if (!pte)
    panic("PTE of swapped page is missing");
  if (*pte & PTE_P)
    panic("PAGE IN REMAP!");
  *pte |= PTE_P | PTE_W | PTE_U;      //Turn on needed bits
  *pte &= ~PTE_PG;                    //Turn off inFile bit
  *pte |= pagePAddr;                  //Map PTE to the new Page
  lcr3(V2P(myproc()->pgdir)); //refresh CR3 register
}

static char buff[PGSIZE];
/*
* Retrieves the paged-out page which its va stored in cr2 from swapfile
* Allocates new room in physical memory for the above purpose
*/
int getPageFromFile(int cr2){

  struct proc* p = myproc();
  // This buffer used to store swapped-in page temporary
  // char buff[PGSIZE];

  p->faultCounter++;
  int userPageVAddr = PGROUNDDOWN(cr2);

  // Allocate new space in memory of page size for the swapping-in page
  char * newPg = kalloc();

  // Initialize the allocated page in memory with 0
  memset(newPg, 0, PGSIZE);

  // Find available page room in memory and return its index in array
  int outIndex = getFreeRamCtrlrIndex();

  // Refresh CR3 register to avoid non-updated address access from TLB
  lcr3(V2P(p->pgdir));

  // If there is a room for a new page in memory..
  if (outIndex >= 0) {
    // Update PTE flags and map userPageVAddr to the physical address newPg
    fixPagedInPTE(userPageVAddr, V2P(newPg), p->pgdir);

    // Find the relevant page (with userPageVAddr) in swapfile, and write its content in the new allocated address in memory
    readPageFromFile(p, outIndex, userPageVAddr, (char*)userPageVAddr);

    return 1; //Operation was successful
  }
  p->countOfPagedOut++;

  /*
  * Swapping-out is needed
  */

  // Find the available page space in swapfile and return its index in array
  outIndex = getPageOutIndex();

  struct pagecontroller outPage = p->ramCtrlr[outIndex];

  // Find the relevant page (with userPageVAddr) in swapfile, and write its content in the new allocated address in memory
  fixPagedInPTE(userPageVAddr, V2P(newPg), p->pgdir);

  // Find the relevant page (with userPageVAddr) in swapfile, and write its content on buff temporary
  readPageFromFile(p, outIndex, userPageVAddr, buff);

  // Get the corresponding physical address of outPage.userPageVAddr
  int outPagePAddr = getPagePAddr(outPage.userPageVAddr, outPage.pgdir);

  // Writes buff into newPg, in other words reads the page from swapfile to physical memory
  memmove(newPg, buff, PGSIZE);

  // Write the swapped-out page from memory to swapfile
  writePageToFile(p, outPage.userPageVAddr, outPage.pgdir);

  // Update outPage.userPageVAddr PTE flags for proper to swapping-out
  fixPagedOutPTE(outPage.userPageVAddr, outPage.pgdir);

  // Get the corresponding physical address of the swapped-out page's virtual address
  char *v = (char*)P2V(outPagePAddr);

  // Free the memory space of the swapped-out page
  kfree(v);

  return 1;
}

/*
* Checks if a policy was defined or not
*/
int isNONEpolicy(){
  #if NONE
    return 1;
  #endif
  return 0;
}

/*
* Updates the accessCouter field of all used pages of process p,
* according to their PTE_A flag (which turned on when access to page ocurred)
*/
void updateAccessCounters(struct proc* p){
  pte_t * pte;
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    if (p->ramCtrlr[i].state == USED){
      pte = walkpgdir(p->ramCtrlr[i].pgdir, (char*)p->ramCtrlr[i].userPageVAddr,0);
      
      p->ramCtrlr[i].accessTracker = p->ramCtrlr[i].accessTracker >> 1; // shift right by 1
      if(*pte & PTE_A){
        
        *pte &= ~PTE_A; // turn off PTE_A flag
        p->ramCtrlr[i].accessTracker |= 0x80000000; // add bit 1 to MSB
      }
      else{
        p->ramCtrlr[i].accessTracker &= 0x7FFFFFFF;
      }

      // cprintf("p->ramCtrlr[%d].accessTracker: %x\n", i, p->ramCtrlr[i].accessTracker);
    } 
  }
}

// void updateAccessCounters(struct proc * p){
//   pte_t * pte;
//   int i;
//   for (i = 0; i < MAX_PSYC_PAGES; i++) {
//     if (p->ramCtrlr[i].state == USED){
//       pte = walkpgdir(p->ramCtrlr[i].pgdir, (char*)p->ramCtrlr[i].userPageVAddr,0);
//       if (*pte & PTE_A) {
//         *pte &= ~PTE_A; // turn off PTE_A flag
//          p->ramCtrlr[i].accessTracker++;
//          cprintf("p->ramCtrlr[%d].accessTracker: %d\n", i, p->ramCtrlr[i].accessTracker);
//       }
//     } 
//   }
// }

void updateAdvQueues(struct proc* p){
  
  int priorPageIndex = -1;
  int afterPageIndex = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ramCtrlr[i].state == NOTUSED)
      continue;

    if(priorPageIndex == -1){ // If this is the first page in ram we meet
      
      priorPageIndex = findNextAQPageIndex(p, priorPageIndex);
    }
    // else if(afterPageIndex == -1){
    //   afterPageIndex = findNextAQPageIndex(p, priorPageIndex);
    //   if(isPageAccessed(p, afterPageIndex) &&
    //     !isPageAccessed(p, priorPageIndex)){

    //     swapAdvQueue(p, afterPageIndex, priorPageIndex);
    //   }

    //   priorPageIndex = afterPageIndex;
    // }
    else{
      afterPageIndex = findNextAQPageIndex(p, priorPageIndex);
      if(isPageAccessed(p, afterPageIndex) &&
        !isPageAccessed(p, priorPageIndex)){

        swapAdvQueue(p, afterPageIndex, priorPageIndex);
      }

      priorPageIndex = afterPageIndex;
    }
  }
}


// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  // If any policy is defined..
  if (!isNONEpolicy()){
    // If number of pages composing newsz exceeds MAX_TOTAL_PAGES and the current proc is NOT init or shell...
    if (PGROUNDUP(newsz)/PGSIZE > MAX_TOTAL_PAGES && myproc()->pid > 2) {
      cprintf("proc is too big\n", PGROUNDUP(newsz)/PGSIZE);
      return 0;
    }
  }


  a = PGROUNDUP(oldsz);

  int i = 0;
  for(; a < newsz; a += PGSIZE){
    
    mem = kalloc();
    i++;
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }

    // If any policy is defined AND current proc is NOT init or shell...
    if (!isNONEpolicy() && myproc()->pid > 2){
      // If current proc cannot have more pages in memory (exceeds MAX_PSYC_PAGES)
      if (PGROUNDUP(oldsz)/PGSIZE + i > MAX_PSYC_PAGES)
        swap(pgdir, a);
      else //there's room
        addToRamCtrlr(pgdir, a);
    }
  }

  return newsz;
}


void removeFromRamCtrlr(uint userPageVAddr, pde_t *pgdir){
  
  struct proc* proc = myproc();

  if (proc == 0)
    return;
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    if (proc->ramCtrlr[i].state == USED 
        && proc->ramCtrlr[i].userPageVAddr == userPageVAddr
        && proc->ramCtrlr[i].pgdir == pgdir){
      proc->ramCtrlr[i].state = NOTUSED;
      return;
    }
  }
}

void removeFromFileCtrlr(uint userPageVAddr, pde_t *pgdir){

  struct proc* proc = myproc();

  if (proc == 0)
    return;
  int i;
  for (i = 0; i < MAX_TOTAL_PAGES-MAX_PSYC_PAGES; i++) {
    if (proc->fileCtrlr[i].state == USED 
        && proc->fileCtrlr[i].userPageVAddr == userPageVAddr
        && proc->fileCtrlr[i].pgdir == pgdir){
      proc->fileCtrlr[i].state = NOTUSED;
      return;
    }
  }
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      if (!isNONEpolicy())
        removeFromRamCtrlr(a, pgdir);
      
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    
    if (*pte & PTE_PG){
      fixPagedOutPTE(i, d);
      continue;
    }


    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");


    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

