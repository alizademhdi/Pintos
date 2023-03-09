#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

struct process_status *
create_process_status (void)
{
  struct process_status *ps = malloc (sizeof(struct process_status));
  ps->ref_count = 2;
  sema_init (&ps->sema, 0);
  lock_init (&ps->lock);

  return ps;
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy, *fn_threadname_copy;
  tid_t tid;

  struct ts *ts_copy = malloc (sizeof (struct ts));

  struct thread *current_thread = thread_current();
  struct process_status *ps = create_process_status();
  ts_copy->ps = ps;

  /* Add the new process to current thread's children elements. */
  list_push_back (&current_thread->children, &ps->elem);

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Make a second copy of FILE_NAME that is used to parse the thread name. Fixes bug causing last line of many tests to have extra characters. */
  fn_threadname_copy = palloc_get_page (0);
  if (fn_threadname_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_threadname_copy, file_name, PGSIZE);

  /* Tokenize first argument of FILE_NAME to be used as the name of the thread. */
  char *save_ptr;
  char *thread_name = strtok_r (fn_threadname_copy, " ", &save_ptr);

  ts_copy->file_name = fn_copy;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (fn_threadname_copy, PRI_DEFAULT, start_process, ts_copy);
  palloc_free_page (fn_threadname_copy);

  /* Wait for the process to fully load. */

  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);
  else
    sema_down(&ps->sema);

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *ts)
{
  struct ts * ts_copy = (struct ts *) ts;
  struct process_status *ps = ts_copy->ps;
  struct thread *current_thread = thread_current ();
  current_thread->tstatus = ps;
  ps->pid = current_thread->tid;

  char *file_name = ts_copy->file_name;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!success)
  {
    ps->exit_code = -1;
    sema_up(&ps->sema);
    thread_exit ();
  }

  sema_up(&ps->sema);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Finds the child process of the given thread with the specified thread ID and returns its `process_status`. */
struct process_status *
get_child_ps (struct thread *t, tid_t child_pid)
{
  // TODO refactor
  struct process_status *ret;
  struct list *t_children = &t->children;
  
  struct list_elem *e;
  struct process_status *current_child;
  for (e = list_begin (t_children); e != list_end (t_children); e = list_next (e))
  {
    current_child = list_entry (e, struct process_status, elem);
    if (current_child->pid == child_pid)
    {
      ret = current_child;
      break;
    }
  }

  return ret;
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid)
{
  // todo already waited?
  /* Get the corresponding child's `process_status`. */
  struct process_status *child_ps = get_child_ps (thread_current (), child_tid);
  
  /* If we didn't find it, we return -1. */
  if (!child_ps)
    return -1;

  /* Wait for the child to exit. */
  sema_down (&child_ps->sema);

  /* Keep child's `exit_code`.
     We do this because we want to free `child_ps` before the return statement. */
  int ec = child_ps->exit_code;

  /* Free up the resources related to this child. */
  list_remove (&child_ps->elem);
  free (child_ps);

  /* Return the child's `exit_code`. */
  return ec;
}

void
free_process_resources (struct thread *t)
{
  struct process_status *ps = t->tstatus;

  lock_acquire (&ps->lock);
  ps->ref_count--;
  lock_release (&ps->lock);

  // todo free `file_descriptors`
  // free (t->file_descriptors)
  // do nothing?

  struct list *children = &t->children;
  struct list_elem *e;
  struct process_status *cur_child;
  for (e = list_begin (children); e != list_end (children); e = list_next (e))
  {
    cur_child = list_entry (e, struct process_status, elem);
    if (cur_child->ref_count == 1)
    {
      e = list_remove (&cur_child->elem)->prev;
      free (cur_child);
    }
  }
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *current_thread = thread_current ();
  uint32_t *pd;

  lock_acquire (&(current_thread->tstatus->lock));
  (current_thread->tstatus->ref_count)--;
  lock_release (&(current_thread->tstatus->lock));
  sema_up (&(current_thread->tstatus->sema));

  if (current_thread->tstatus->ref_count == 0)
    free (current_thread->tstatus);

  //free_process_resources(current_thread);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = current_thread->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      current_thread->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
  file_close(current_thread->file_exec);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, char *argv[], int argc);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Limits for filename. */
#define MAX_ARGS 32
#define MAX_FILENAME_SIZE 1024
                          
bool parse_file_name (char *file_name, int *argc, char **argv);

/* Parses filename into array of strings (argv). */
bool
parse_file_name (char *file_name, int *argc, char **argv)
{
  int i;
  char *c;
  
  char *saveptr;

  c = strtok_r(file_name," ", &saveptr);	 /* Start tokenizer on filename */
  for (i=0; c && i < MAX_ARGS; i++) {
    argv[i] = c;
    c = strtok_r(NULL," ", &saveptr);	/* scan for next token */
  }

  if (strtok_r(NULL, " ", &saveptr) != NULL)
    return false;

  *argc = i;

  return true;
}

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  char *file_name_copy = palloc_get_page(0); 
  char **argv = palloc_get_page(0);
  strlcpy(file_name_copy, file_name, PGSIZE);
  int argc;
  if (!parse_file_name (file_name_copy, &argc, argv))
    goto done;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  

  /* Open executable file. */
  file = filesys_open (argv[0]);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", argv[0]);
      goto done;
    }

  /* deny write to executable */
  file_deny_write(file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", argv[0]);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }
// 
  /* Set up stack. */
  if (!setup_stack (esp, argv, argc))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  palloc_free_page(file_name_copy);
  palloc_free_page(argv);
  t->file_exec = file;
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, char *argv[], int argc)
{
  uint8_t *kpage;
  bool success = false;

  uint8_t *arg_address[argc];

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success) {
        *esp = PHYS_BASE;
      } else {
        palloc_free_page (kpage);
        goto setup_done;
      }
    }
  
  uint8_t *stack_page_ptr = (uint8_t *) *esp;
  // size_t arglen;

  /* pushing the elements of argv onto the stack. */
  for (int i = argc - 1; i >= 0; i--) {
    size_t arglen = strlen(argv[i]) + 1;
    stack_page_ptr -= arglen;
    strlcpy ((char *) stack_page_ptr, argv[i], arglen);
    arg_address[i] = stack_page_ptr;
  }

  /* initial 4 byte alignment. */
  stack_page_ptr -= ((uint32_t)stack_page_ptr) % 4;

  /* terminating NULL address. (see page 14 of instruction document)*/
  stack_page_ptr -= 4;
  *((uint32_t*)stack_page_ptr) = (uint32_t) 0;

  /* pushing the addresses of argv elements onto the stack. */
  for (int i = argc - 1; i >= 0; i--) {
    stack_page_ptr -= 4;
    *((uint32_t*)stack_page_ptr) = (uint32_t) arg_address[i];
  }  

  /* second alignment; this time 16 bytes. */
  uint32_t temporary = (uint32_t) stack_page_ptr;
  stack_page_ptr -= (((uint32_t) stack_page_ptr) + 8) % 16;

  /* pushing &argv onto stack. */
  stack_page_ptr -= 4;
  *((uint32_t*)stack_page_ptr) = temporary;

  /* pushing argc onto stack. */
  stack_page_ptr -= 4;
  *((uint32_t*)stack_page_ptr) = (uint32_t) argc;

  /* pushing (fake) return address 0. */
  stack_page_ptr -= 4;
  *((uint32_t*)stack_page_ptr) = (uint32_t) 0;

  *esp = (void *) stack_page_ptr;

setup_done:    
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
