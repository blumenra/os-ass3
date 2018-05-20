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


/*
* Gets the index of page in memory which should be swapped-out according to the defined policy
*/
int find_avail_page_index_to_swapout(struct proc* p){
  #if NFUA
    return find_avail_index_by_NFUA(p);
  #endif
  #if LAPA
    return find_avail_index_by_LAPA(p);
  #endif
  #if SCFIFO
    return find_avail_index_by_SCFIFO(p);
  #endif
  #if AQ
    return find_avail_index_by_AQ(p);
  #endif
  panic("find_avail_page_index_to_swapout: policy error");
}

int find_avail_index_by_NFUA(struct proc* p){

  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ram_manager[i].state == NOT_USED)
      continue;
    
    if((min == -1) || (p->ram_manager[min].access_tracker > p->ram_manager[i].access_tracker)){
      min = i;
    }
  }

  return min;
}

int find_avail_index_by_LAPA(struct proc* p){

  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ram_manager[i].state == NOT_USED)
      continue;

    if((min == -1) || (countNumOfOneBits(p->ram_manager[min].access_tracker) > countNumOfOneBits(p->ram_manager[i].access_tracker))){
      min = i;
    }
  }

  return min;
}


int find_avail_index_by_SCFIFO(struct proc* p){

  pte_t* pte;
  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ram_manager[i].state == NOT_USED)
      continue;

    if((min == -1) || (p->ram_manager[min].create_order > p->ram_manager[i].create_order)){
      min = i;
    }
  }

  pte = walkpgdir(p->ram_manager[min].pgdir, (char*)p->ram_manager[min].vAddr,0);
  
  // If the page was accessed..
  if (*pte & PTE_A) {
    *pte &= ~PTE_A; // turn off PTE_A flag
     p->ram_manager[min].create_order = generate_creation_number(p); // update page's create_order to be the highest (treat it like it was just created)
     return find_avail_index_by_SCFIFO(p);
  }

  return min;
}

int find_avail_index_by_AQ(struct proc* p){

  int min = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ram_manager[i].state == NOT_USED)
      continue;

    if((min == -1) || (p->ram_manager[min].adv_queue > p->ram_manager[i].adv_queue)){
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
    ans = findNextAdvPageIndex(p, p->ram_manager[index].adv_queue);
  }
  else{ // Find the first in advQueu
    ans = findMinAdvPageIndex(p);
  }

  return ans;
}

int findNextAdvPageIndex(struct proc* p, int boundery){

  uint min = -1;
  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ram_manager[i].state == NOT_USED)
      continue;
    if(p->ram_manager[i].adv_queue <= boundery)
      continue;

    // Here p->ram_manager[i] is in the ram and grater than boundery
    if((min < 0) || (p->ram_manager[min].adv_queue > p->ram_manager[i].adv_queue)){
      min = i;
    }
  }

  return min;
}


int findMinAdvPageIndex(struct proc* p){
  return findNextAdvPageIndex(p, 0xFFFFFFFF);
}

void swap_adv_queue(struct proc* p, int priorPageIndex, int afterPageIndex){

  int tmpTurn = 0;
  tmpTurn = p->ram_manager[priorPageIndex].adv_queue;
  p->ram_manager[priorPageIndex].adv_queue = p->ram_manager[afterPageIndex].adv_queue;
  p->ram_manager[afterPageIndex].adv_queue = tmpTurn;
}

int isPageAccessed(struct proc* p, int index){

  pte_t* pte = walkpgdir(p->ram_manager[index].pgdir, (char*)(p->ram_manager[index].vAddr), 0);
  return (*pte & PTE_A);
}

/*
* Returns the pysical address mapped to the virtual address vAddr in the page-dir
*/
int acquire_pAddr(int vAddr, pde_t * pgdir){

  pte_t* pte = walkpgdir(pgdir, (int*)vAddr, 0);

  // Get the a pointer to the PTE of vAddr in pgdir (Dont allocate new PTE if didnt found.. (third parameter))
  for(int i=0; i < PGSIZE*PGSIZE/4; i++)
    pte = walkpgdir(pgdir, (int*)vAddr, 0);

  if(!pte)
    return -1;

  return PTE_ADDR(*pte);
}

/*
* Change PTE flags properly after swapping-out vAddr
*/
void update_pageOUT_pte_flags(struct proc* p, int vAddr, pde_t * pgdir){
  
  // struct proc* p = myproc();
  
  pte_t *pte = walkpgdir(pgdir, (int*)vAddr, 0);
  if (!pte)
    panic("update_pageOUT_pte_flags: pte does NOT exist in pgdir");

  *pte |= PTE_PG;           // Inidicates that the page was Paged-out to secondary storage
  *pte &= ~PTE_P;           // Indicates that the page is NOT in physical memory
  *pte &= PTE_FLAGS(*pte);
  
  lcr3(V2P(p->pgdir));      // Refresh CR3 register (TLB (cache))
}

/*
* Checks if page corresponding to vAddr is indeed in swapfile (e.g not in memory)
*/
int is_page_in_file(struct proc* p, int vAddr) {
  pte_t *pte;
  pte = walkpgdir(p->pgdir, (char *)vAddr, 0);
  return (*pte & PTE_PG);
}

/*
* Finds an available room for page in memory and returns its index
*/
// int find_avail_index_in_ram_manger(struct proc* p) {
int find_avail_index_in_ram_manger(struct proc* p) {
  
  // struct proc* p = myproc();
  if (p == 0)
    return -1;

  for (int i=0; i < MAX_PSYC_PAGES; i++) {
    if (p->ram_manager[i].state == NOT_USED)
      return i;
  }

  // If got here, it means that no room for pages in memory is left
  return -1;
}

/*
* Finds an available page in memory and updates its virtual address to vAddr, etc.
*/
void add_page_to_ram(struct proc* p, pde_t *pgdir, uint vAddr) {

  int index = find_avail_index_in_ram_manger(p);
  p->ram_manager[index].state = USED;
  p->ram_manager[index].pgdir = pgdir;
  p->ram_manager[index].vAddr = vAddr;
  p->ram_manager[index].create_order = generate_creation_number(p);

  // Initialize access_trackers of all pages on proc np to 0
  int initValue = 0;

  #if NFUA
    initValue = 0;
  #endif

  #if LAPA
    initValue = 0xFFFFFFFF;
  #endif
  p->ram_manager[index].access_tracker = initValue;

  p->ram_manager[index].adv_queue = generate_adv_number(p);
}



/*
* Swaps a sigle page between memory and file by finding the page to swap-out (according to the policy),
* finding a available place in file for the above and put it in it.
* After that, 
*
*/
void swap(struct proc *p, pde_t *pgdir, uint vAddr){
  
  p->paged_out_count++;

  // Get the index of page in memory which should be swapped out according to the policy
  int page_index = find_avail_page_index_to_swapout(p);

  // Get the physical address mapped to the virtual address p->ram_manager[page_index].vAddr in page directory p->ram_manager[page_index].pgdir
  int page_phys_addr = acquire_pAddr(p->ram_manager[page_index].vAddr, p->ram_manager[page_index].pgdir);

  // Swap-out page starting in p->ram_manager[page_index].vAddr
  page_out(p, p->ram_manager[page_index].vAddr, p->ram_manager[page_index].pgdir);
  
  // Converts physical address to virtual address
  char *va = (char*)P2V(page_phys_addr);
  
  //free swapped-out page
  kfree(va);

  // Change state of swapped-out page in MEMORY to UNUSED
  p->ram_manager[page_index].state = NOT_USED;

  // Fix PTE flags properly after swapping-out vAddr
  update_pageOUT_pte_flags(p, p->ram_manager[page_index].vAddr, p->ram_manager[page_index].pgdir);

  // Finds an available page in memory and updates its virtual address to be vAddr
  add_page_to_ram(p, pgdir, vAddr);
}

/*
* Updates PTE flags of vAddr after swapping-in a page
*/
void update_pageIN_pte_flags(struct proc* p, int vAddr, int pagePAddr, pde_t * pgdir){
  
  pte_t *pte = walkpgdir(pgdir, (int*)vAddr, 0);
  
  if (!pte)
    panic("update_pageIN_pte_flags: pte does NOT exist in pgdir");

  if (*pte & PTE_P)
    panic("update_pageIN_pte_flags: page is already in memory!");
  
  *pte |= PTE_P | PTE_W | PTE_U;      //Turn on needed bits
  *pte &= ~PTE_PG;                    //Turn off inFile bit
  *pte |= pagePAddr;                  //Map PTE to the new Page
  lcr3(V2P(p->pgdir)); //refresh CR3 register
}

static char buff[PGSIZE];
/*
* Reads a page corresponding to page_index from swapfile,
* Allocates new room in physical memory for it and writes it into memory
*/
int swap_in(struct proc* p, int page_index){

  // This buffer used to store swapped-in page temporary

  p->page_fault_count++;
  int vAddr = PGROUNDDOWN(page_index);

  // Allocate new space in memory of page size for the swapping-in page
  char* new_allocated_page = kalloc();

  // Initialize the allocated page in memory with 0
  memset(new_allocated_page, 0, PGSIZE);

  // Find available page room in memory and return its index in array
  int avail_index_page_in_ram = find_avail_index_in_ram_manger(p);

  // Refresh CR3 register to avoid non-updated address access from TLB
  lcr3(V2P(p->pgdir));

  // If there is a room for a new page in memory..
  if (avail_index_page_in_ram >= 0) {
    // Update PTE flags and map vAddr to the physical address new_allocated_page
    update_pageIN_pte_flags(p, vAddr, V2P(new_allocated_page), p->pgdir);

    // Find the relevant page (with vAddr) in swapfile, and write its content in the new allocated address in memory
    page_in(p, avail_index_page_in_ram, vAddr, (char*)vAddr);

    return 1; //Operation was successful
  }
  p->paged_out_count++;

  /*
  * Swapping-out is needed
  */

  // Find the available page space in swapfile and return its index in array
  avail_index_page_in_ram = find_avail_page_index_to_swapout(p);

  struct page_struct outPage = p->ram_manager[avail_index_page_in_ram];

  // Find the relevant page (with vAddr) in swapfile, and write its content in the new allocated address in memory
  update_pageIN_pte_flags(p, vAddr, V2P(new_allocated_page), p->pgdir);

  // Find the relevant page (with vAddr) in swapfile, and write its content on buff temporary
  page_in(p, avail_index_page_in_ram, vAddr, buff);

  // Get the corresponding physical address of outPage.vAddr
  int outPagePAddr = acquire_pAddr(outPage.vAddr, outPage.pgdir);

  // Writes buff into new_allocated_page, in other words reads the page from swapfile to physical memory
  memmove(new_allocated_page, buff, PGSIZE);

  // Write the swapped-out page from memory to swapfile
  page_out(p, outPage.vAddr, outPage.pgdir);

  // Update outPage.vAddr PTE flags for proper to swapping-out
  update_pageOUT_pte_flags(p, outPage.vAddr, outPage.pgdir);

  // Get the corresponding physical address of the swapped-out page's virtual address
  char *v = (char*)P2V(outPagePAddr);

  // Free the memory space of the swapped-out page
  kfree(v);

  return 1;
}

/*
* Checks if a policy was defined or not
*/
int check_NONE_policy(void){
  #if NONE
    return 1;
  #endif
  return 0;
}

/*
* Updates the accessCouter field of all used pages of process p,
* according to their PTE_A flag (which turned on when access to page ocurred)
*/
void update_access_trackers(struct proc* p){
  pte_t * pte;
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    if (p->ram_manager[i].state == USED){
      pte = walkpgdir(p->ram_manager[i].pgdir, (char*)p->ram_manager[i].vAddr,0);
      
      p->ram_manager[i].access_tracker = p->ram_manager[i].access_tracker >> 1; // shift right by 1
      if(*pte & PTE_A){
        
        *pte &= ~PTE_A; // turn off PTE_A flag
        p->ram_manager[i].access_tracker |= 0x80000000; // add bit 1 to MSB
      }
      else{
        p->ram_manager[i].access_tracker &= 0x7FFFFFFF;
      }
    } 
  }
}


void update_adv_queues(struct proc* p){
  
  int priorPageIndex = -1;
  int afterPageIndex = -1;

  for(int i=0; i < MAX_PSYC_PAGES; i++){
    if(p->ram_manager[i].state == NOT_USED)
      continue;

    if(priorPageIndex == -1){ // If this is the first page in ram we meet
      
      priorPageIndex = findNextAQPageIndex(p, priorPageIndex);
    }
    else{
      afterPageIndex = findNextAQPageIndex(p, priorPageIndex);
      if(isPageAccessed(p, afterPageIndex) &&
        !isPageAccessed(p, priorPageIndex)){

        swap_adv_queue(p, afterPageIndex, priorPageIndex);
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
  struct proc* p = myproc();

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  // If any policy is defined..
  if (!check_NONE_policy()){
    // If number of pages composing newsz exceeds MAX_TOTAL_PAGES and the current proc is NOT init or shell...
    if (PGROUNDUP(newsz)/PGSIZE > MAX_TOTAL_PAGES && !is_shell_or_init(p)) {
      return 0;
    }
  }


  a = PGROUNDUP(oldsz);

  int addPages = 0;
  for(; a < newsz; a += PGSIZE){
    
    mem = kalloc();
    addPages++;
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
    if (!check_NONE_policy() && !is_shell_or_init(p)){
      // If current proc cannot have more pages in memory (exceeds MAX_PSYC_PAGES)
      if (PGROUNDUP(oldsz)/PGSIZE + addPages > MAX_PSYC_PAGES)
        swap(p, pgdir, a);
      else //there's room
        add_page_to_ram(p, pgdir, a);
    }
  }

  return newsz;
}


void remove_page_from_ram(struct proc* p, uint vAddr, pde_t *pgdir){
  
  if (p == 0)
    return;

  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    if (p->ram_manager[i].state == USED 
        && p->ram_manager[i].vAddr == vAddr
        && p->ram_manager[i].pgdir == pgdir){
      p->ram_manager[i].state = NOT_USED;
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
  struct proc* p = myproc();

  if(p == 0)
    return oldsz;
  
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
      if (!check_NONE_policy())
        remove_page_from_ram(p, a, pgdir);
      
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
  struct proc* p = myproc();
  if(p == 0)
    return 0;

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
      update_pageOUT_pte_flags(p, i, d);
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

