/* mmap.cc

   Copyright 1996, 1997, 1998, 2000, 2001, 2002, 2003, 2004, 2005
   Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/mman.h>
#include "cygerrno.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "pinfo.h"
#include "sys/cygwin.h"
#include "ntdll.h"

#define PAGE_CNT(bytes) howmany((bytes),getpagesize())

#define PGBITS		(sizeof (DWORD)*8)
#define MAPSIZE(pages)	howmany ((pages), PGBITS)

#define MAP_SET(n)	(page_map[(n)/PGBITS] |= (1L << ((n) % PGBITS)))
#define MAP_CLR(n)	(page_map[(n)/PGBITS] &= ~(1L << ((n) % PGBITS)))
#define MAP_ISSET(n)	(page_map[(n)/PGBITS] & (1L << ((n) % PGBITS)))

/* Used for accessing the page file (anonymous mmaps). */
static fhandler_dev_zero fh_paging_file;

/* Small helpers to avoid having lots of flag bit tests in the code. */
static inline bool
priv (int flags)
{
  return (flags & MAP_PRIVATE) == MAP_PRIVATE;
}

static inline bool
fixed (int flags)
{
  return (flags & MAP_FIXED) == MAP_FIXED;
}

static inline bool
anonymous (int flags)
{
  return (flags & MAP_ANONYMOUS) == MAP_ANONYMOUS;
}

static inline bool
noreserve (int flags)
{
  return (flags & MAP_NORESERVE) == MAP_NORESERVE;
}

static inline bool
autogrow (int flags)
{
  return (flags & MAP_AUTOGROW) == MAP_AUTOGROW;
}

/* Generate Windows protection flags from mmap prot and flag values. */
static DWORD
gen_protect (int prot, int flags, bool create = false)
{
  DWORD ret = PAGE_NOACCESS;
  /* When creating a private map/section, the protection must be set to
     PAGE_WRITECOPY, otherwise the page protection can't be set to
     PAGE_WRITECOPY in later calls to VirtualProtect.  This does not
     hold for private anonymous maps, since these are mapped using
     VirtualAlloc.  The PAGE_WRITECOPY protection is never used for
     them. */
  if (create && priv (flags) && !anonymous (flags))
    ret = PAGE_WRITECOPY;
  else if (prot & PROT_WRITE)
    {
      /* Windows doesn't support write without read. */
      ret <<= 2;
      if (priv (flags) && !anonymous (flags))
	ret <<= 1;
    }
  else if (prot & PROT_READ)
    ret <<= 1;
  /* Ignore EXECUTE permission on 9x. */
  if ((prot & PROT_EXEC)
      && wincap.virtual_protect_works_on_shared_pages ())
    ret <<= 4;
  return ret;
}

/* Generate Windows access flags from mmap prot and flag values.
   Only used on 9x.  PROT_EXEC not supported here since it's not
   necessary. */
static DWORD
gen_access (int prot, int flags)
{
  DWORD ret = 0;
  if (priv (flags))
    ret = FILE_MAP_COPY;
  else if (prot & PROT_WRITE)
    ret = priv (flags) ? FILE_MAP_COPY : FILE_MAP_WRITE;
  else if (prot & PROT_READ)
    ret = FILE_MAP_READ;
  return ret;
}

/* OS specific wrapper functions for map/section functions. */
static BOOL
VirtualProt9x (PVOID addr, SIZE_T len, DWORD prot, PDWORD oldprot)
{
  if (addr >= (caddr_t)0x80000000 && addr <= (caddr_t)0xBFFFFFFF)
    return TRUE; /* FAKEALARM! */
  return VirtualProtect (addr, len, prot, oldprot);
}

static BOOL
VirtualProtNT (PVOID addr, SIZE_T len, DWORD prot, PDWORD oldprot)
{
  return VirtualProtect (addr, len, prot, oldprot);
}

static BOOL
VirtualProtEx9x (HANDLE parent, PVOID addr, SIZE_T len, DWORD prot,
		 PDWORD oldprot)
{
  if (addr >= (caddr_t)0x80000000 && addr <= (caddr_t)0xBFFFFFFF)
    return TRUE; /* FAKEALARM! */
  return VirtualProtectEx (parent, addr, len, prot, oldprot);
}
static BOOL
VirtualProtExNT (HANDLE parent, PVOID addr, SIZE_T len, DWORD prot,
		 PDWORD oldprot)
{
  return VirtualProtectEx (parent, addr, len, prot, oldprot);
}

/* This allows to stay lazy about VirtualProtect usage in subsequent code. */
#define VirtualProtect(a,l,p,o) (mmap_func->VirtualProt((a),(l),(p),(o)))
#define VirtualProtectEx(h,a,l,p,o) (mmap_func->VirtualProtEx((h),(a),(l),(p),(o)))

static HANDLE
CreateMapping9x (HANDLE fhdl, size_t len, _off64_t off, int prot, int flags,
		 const char *name)
{
  HANDLE h;
  DWORD high, low;

  DWORD protect = gen_protect (prot, flags, true);

  /* copy-on-write doesn't work properly on 9x with real files.  While the
     changes are not propagated to the file, they are visible to other
     processes sharing the same file mapping object.  Workaround: Don't
     use named file mapping.  That should work since sharing file
     mappings only works reliable using named file mapping on 9x.

     On 9x/ME try first to open the mapping by name when opening a
     shared file object. This is needed since 9x/ME only shares objects
     between processes by name. What a mess... */

  if (fhdl != INVALID_HANDLE_VALUE && !priv (flags))
    {
      /* Grrr, the whole stuff is just needed to try to get a reliable
	 mapping of the same file. Even that uprising isn't bullet
	 proof but it does it's best... */
      char namebuf[CYG_MAX_PATH];
      cygwin_conv_to_full_posix_path (name, namebuf);
      for (int i = strlen (namebuf) - 1; i >= 0; --i)
	namebuf[i] = cyg_tolower (namebuf [i]);

      debug_printf ("named sharing");
      DWORD access = gen_access (prot, flags);
      if (!(h = OpenFileMapping (access, TRUE, namebuf)))
	h = CreateFileMapping (fhdl, &sec_none, protect, 0, 0,
			       namebuf);
    }
  else if (fhdl == INVALID_HANDLE_VALUE)
    {
      /* Standard anonymous mapping needs non-zero len. */
      h = CreateFileMapping (fhdl, &sec_none, protect, 0, len, NULL);
    }
  else if (autogrow (flags))
    {
      high = (off + len) >> 32;
      low = (off + len) & UINT32_MAX;
      /* Auto-grow only works if the protection is PAGE_READWRITE.  So,
         first we call CreateFileMapping with PAGE_READWRITE, then, if the
	 requested protection is different, we close the mapping and
	 reopen it again with the correct protection, if auto-grow worked. */
      h = CreateFileMapping (fhdl, &sec_none, PAGE_READWRITE,
			     high, low, NULL);
      if (h && protect != PAGE_READWRITE)
	{
	  CloseHandle (h);
	  h = CreateFileMapping (fhdl, &sec_none, protect,
				 high, low, NULL);
	}
    }
  else
    {
      /* Zero len creates mapping for whole file. */
      h = CreateFileMapping (fhdl, &sec_none, protect, 0, 0, NULL);
    }
  return h;
}

static HANDLE
CreateMappingNT (HANDLE fhdl, size_t len, _off64_t off, int prot, int flags,
		 const char *)
{
  HANDLE h;
  NTSTATUS ret;

  LARGE_INTEGER sectionsize = { QuadPart: len };
  ULONG protect = gen_protect (prot, flags, true);
  ULONG attributes = SEC_COMMIT;	/* For now! */

  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes (&oa, NULL, OBJ_INHERIT, NULL,
			      sec_none.lpSecurityDescriptor);

  if (fhdl == INVALID_HANDLE_VALUE)
    {
      /* Standard anonymous mapping needs non-zero len. */
      ret = NtCreateSection (&h, SECTION_ALL_ACCESS, &oa,
			     &sectionsize, protect, attributes, NULL);
    }
  else if (autogrow (flags))
    {
      /* Auto-grow only works if the protection is PAGE_READWRITE.  So,
         first we call NtCreateSection with PAGE_READWRITE, then, if the
	 requested protection is different, we close the mapping and
	 reopen it again with the correct protection, if auto-grow worked. */
      sectionsize.QuadPart += off;
      ret = NtCreateSection (&h, SECTION_ALL_ACCESS, &oa,
			     &sectionsize, PAGE_READWRITE, attributes, fhdl);
      if (NT_SUCCESS (ret) && protect != PAGE_READWRITE)
        {
	  CloseHandle (h);
	  ret = NtCreateSection (&h, SECTION_ALL_ACCESS, &oa,
				 &sectionsize, protect, attributes, fhdl);
	}
    }
  else
    {
      /* Zero len creates mapping for whole file and allows
         AT_EXTENDABLE_FILE mapping, if we ever use it... */
      sectionsize.QuadPart = 0;
      ret = NtCreateSection (&h, SECTION_ALL_ACCESS, &oa,
			     &sectionsize, protect, attributes, fhdl);
    }
  if (!NT_SUCCESS (ret))
    {
      h = NULL;
      SetLastError (RtlNtStatusToDosError (ret));
    }
  return h;
}

void *
MapView9x (HANDLE h, void *addr, size_t len, int prot, int flags, _off64_t off)
{
  DWORD high = off >> 32;
  DWORD low = off & UINT32_MAX;
  DWORD access = gen_access (prot, flags);
  void *base;

  /* Try mapping using the given address first, even if it's NULL.
     If it failed, and addr was not NULL and flags is not MAP_FIXED,
     try again with NULL address. */
  if (!addr)
    base = MapViewOfFile (h, access, high, low, len);
  else
    {
      base = MapViewOfFileEx (h, access, high, low, len, addr);
      if (!base && !fixed (flags))
	base = MapViewOfFile (h, access, high, low, len);
    }
  debug_printf ("%x = MapViewOfFileEx (h:%x, access:%x, 0, off:%D, "
		"len:%u, addr:%x)", base, h, access, off, len, addr);
  return base;
}

void *
MapViewNT (HANDLE h, void *addr, size_t len, int prot, int flags, _off64_t off)
{
  NTSTATUS ret;
  LARGE_INTEGER offset = { QuadPart:off };
  DWORD protect = gen_protect (prot, flags, true);
  void *base = addr;
  ULONG size = len;

  /* Try mapping using the given address first, even if it's NULL.
     If it failed, and addr was not NULL and flags is not MAP_FIXED,
     try again with NULL address. */
  ret = NtMapViewOfSection (h, GetCurrentProcess (), &base, 0, size, &offset,
			    &size, ViewShare, 0, protect);
  if (!NT_SUCCESS (ret) && addr  && !fixed (flags))
    {
      base = NULL;
      ret = NtMapViewOfSection (h, GetCurrentProcess (), &base, 0, size,
      				&offset, &size, ViewShare, 0, protect);
    }
  if (!NT_SUCCESS (ret))
    {
      base = NULL;
      SetLastError (RtlNtStatusToDosError (ret));
    }
  debug_printf ("%x = NtMapViewOfSection (h:%x, addr:%x 0, len:%u, off:%D, "
		"protect:%x,)", base, h, addr, len, off, protect);
  return base;
}

struct mmap_func_t
{
  HANDLE (*CreateMapping)(HANDLE, size_t, _off64_t, int, int, const char*);
  void * (*MapView)(HANDLE, void *, size_t, int, int, _off64_t);
  BOOL	 (*VirtualProt)(PVOID, SIZE_T, DWORD, PDWORD);
  BOOL	 (*VirtualProtEx)(HANDLE, PVOID, SIZE_T, DWORD, PDWORD);
};

mmap_func_t mmap_funcs_9x = 
{
  CreateMapping9x,
  MapView9x,
  VirtualProt9x,
  VirtualProtEx9x
};

mmap_func_t mmap_funcs_nt = 
{
  CreateMappingNT,
  MapViewNT,
  VirtualProtNT,
  VirtualProtExNT
};

mmap_func_t *mmap_func;

void
mmap_init ()
{
  mmap_func = wincap.is_winnt () ? &mmap_funcs_nt : &mmap_funcs_9x;
}

/* Class structure used to keep a record of all current mmap areas
   in a process.  Needed for bookkeeping all mmaps in a process and
   for duplicating all mmaps after fork() since mmaps are not propagated
   to child processes by Windows.  All information must be duplicated
   by hand, see fixup_mmaps_after_fork().

   The class structure:

   One member of class map per process, global variable mmapped_areas.
   Contains a dynamic class list array.  Each list entry represents all
   mapping to a file, keyed by file descriptor and file name hash.
   Each list entry contains a dynamic class mmap_record array.  Each
   mmap_record represents exactly one mapping.  For each mapping, there's
   an additional so called `page_map'.  It's an array of bits, one bit
   per mapped memory page.  The bit is set if the page is accessible,
   unset otherwise. */

class mmap_record
{
  private:
    int fd;
    HANDLE mapping_hdl;
    int prot;
    int flags;
    _off64_t offset;
    DWORD len;
    caddr_t base_address;
    DWORD *page_map;
    device dev;

  public:
    mmap_record (int nfd, HANDLE h, int p, int f, _off64_t o, DWORD l,
    		 caddr_t b) :
       fd (nfd),
       mapping_hdl (h),
       prot (p),
       flags (f),
       offset (o),
       len (l),
       base_address (b),
       page_map (NULL)
      {
	dev.devn = 0;
	if (fd >= 0 && !cygheap->fdtab.not_open (fd))
	  dev = cygheap->fdtab[fd]->dev ();
	else if (fd == -1)
	  dev.parse (FH_ZERO);
      }

    int get_fd () const { return fd; }
    HANDLE get_handle () const { return mapping_hdl; }
    device& get_device () { return dev; }
    int get_prot () const { return prot; }
    int get_flags () const { return flags; }
    bool priv () const { return ::priv (flags); }
    bool fixed () const { return ::fixed (flags); }
    bool anonymous () const { return ::anonymous (flags); }
    bool noreserve () const { return ::noreserve (flags); }
    bool autogrow () const { return ::autogrow (flags); }
    _off64_t get_offset () const { return offset; }
    DWORD get_len () const { return len; }
    caddr_t get_address () const { return base_address; }

    bool alloc_page_map ();
    void free_page_map () { if (page_map) cfree (page_map); }

    DWORD find_unused_pages (DWORD pages) const;
    _off64_t map_pages (_off64_t off, DWORD len);
    bool map_pages (caddr_t addr, DWORD len);
    bool unmap_pages (caddr_t addr, DWORD len);
    int access (caddr_t address);

    fhandler_base *alloc_fh ();
    void free_fh (fhandler_base *fh);
    
    DWORD gen_protect () const
      { return ::gen_protect (get_prot (), get_flags ()); }
    DWORD gen_access () const
      { return ::gen_access (get_prot (), get_flags ()); }
    bool compatible_flags (int fl) const;
};

class list
{
  private:
    mmap_record *recs;
    int nrecs, maxrecs;
    int fd;
    DWORD hash;

  public:
    int get_fd () const { return fd; }
    DWORD get_hash () const { return hash; }
    mmap_record *get_record (int i) { return i >= nrecs ? NULL : recs + i; }

    bool anonymous () const { return fd == -1; }
    void set (int nfd);
    mmap_record *add_record (mmap_record r);
    bool del_record (int i);
    void free_recs () { if (recs) cfree (recs); }
    mmap_record *search_record (_off64_t off, DWORD len);
    long search_record (caddr_t addr, DWORD len, caddr_t &m_addr, DWORD &m_len,
		long start);
};

class map
{
  private:
    list *lists;
    unsigned nlists, maxlists;

  public:
    list *get_list (unsigned i) { return i >= nlists ? NULL : lists + i; }
    list *get_list_by_fd (int fd);
    list *add_list (int fd);
    void del_list (unsigned i);
};

/* This is the global map structure pointer.  It's allocated once on the
   first call to mmap64(). */
static map mmapped_areas;

bool
mmap_record::compatible_flags (int fl) const
{
#define MAP_COMPATMASK	(MAP_TYPE | MAP_NORESERVE)
  return (get_flags () & MAP_COMPATMASK) == (fl & MAP_COMPATMASK);
}

DWORD
mmap_record::find_unused_pages (DWORD pages) const
{
  DWORD mapped_pages = PAGE_CNT (get_len ());
  DWORD start;

  if (pages > mapped_pages)
    return (DWORD)-1;
  for (start = 0; start <= mapped_pages - pages; ++start)
    if (!MAP_ISSET (start))
      {
	DWORD cnt;
	for (cnt = 0; cnt < pages; ++cnt)
	  if (MAP_ISSET (start + cnt))
	    break;
	if (cnt >= pages)
	  return start;
      }
  return (DWORD)-1;
}

bool
mmap_record::alloc_page_map ()
{
  /* Allocate one bit per page */
  if (!(page_map = (DWORD *) ccalloc (HEAP_MMAP,
				      MAPSIZE (PAGE_CNT (get_len ())),
				      sizeof (DWORD))))
    return false;

  DWORD old_prot;
  DWORD len = PAGE_CNT (get_len ());
  DWORD protect = gen_protect ();
  if (protect != PAGE_WRITECOPY && priv () && !anonymous ()
      && !VirtualProtect (get_address (), len * getpagesize (),
      			  protect, &old_prot))
    syscall_printf ("VirtualProtect(%x,%D,%d) failed, %E",
		    get_address (), len * getpagesize ());
  while (len-- > 0)
    MAP_SET (len);
  return true;
}

_off64_t
mmap_record::map_pages (_off64_t off, DWORD len)
{
  /* Used ONLY if this mapping matches into the chunk of another already
     performed mapping in a special case of MAP_ANON|MAP_PRIVATE.

     Otherwise it's job is now done by alloc_page_map(). */
  DWORD old_prot;
  debug_printf ("map_pages (fd=%d, off=%D, len=%u)", get_fd (), off, len);
  len = PAGE_CNT (len);

  if ((off = find_unused_pages (len)) == (DWORD)-1)
    return 0L;
  if (!noreserve ()
      && !VirtualProtect (get_address () + off * getpagesize (),
			  len * getpagesize (), gen_protect (), &old_prot))
    {
      __seterrno ();
      return (_off64_t)-1;
    }

  while (len-- > 0)
    MAP_SET (off + len);
  return off * getpagesize ();
}

bool
mmap_record::map_pages (caddr_t addr, DWORD len)
{
  debug_printf ("map_pages (addr=%x, len=%u)", addr, len);
  DWORD old_prot;
  DWORD off = addr - get_address ();
  off /= getpagesize ();
  len = PAGE_CNT (len);
  /* First check if the area is unused right now. */
  for (DWORD l = 0; l < len; ++l)
    if (MAP_ISSET (off + l))
      {
	set_errno (EINVAL);
	return false;
      }
  if (!noreserve ()
      && !VirtualProtect (get_address () + off * getpagesize (),
			  len * getpagesize (), gen_protect (), &old_prot))
    {
      __seterrno ();
      return false;
    }
  for (; len-- > 0; ++off)
    MAP_SET (off);
  return true;
}

bool
mmap_record::unmap_pages (caddr_t addr, DWORD len)
{
  DWORD old_prot;
  DWORD off = addr - get_address ();
  off /= getpagesize ();
  len = PAGE_CNT (len);
  if (anonymous () && priv () && noreserve ()
      && !VirtualFree (get_address () + off * getpagesize (),
		       len * getpagesize (), MEM_DECOMMIT))
    syscall_printf ("VirtualFree in unmap_pages () failed, %E");
  else if (!VirtualProtect (get_address () + off * getpagesize (),
			    len * getpagesize (), PAGE_NOACCESS, &old_prot))
    syscall_printf ("VirtualProtect in unmap_pages () failed, %E");

  for (; len-- > 0; ++off)
    MAP_CLR (off);
  /* Return TRUE if all pages are free'd which may result in unmapping
     the whole chunk. */
  for (len = MAPSIZE (PAGE_CNT (get_len ())); len > 0; )
    if (page_map[--len])
      return false;
  return true;
}

int
mmap_record::access (caddr_t address)
{
  if (address < get_address () || address >= get_address () + get_len ())
    return 0;
  DWORD off = (address - get_address ()) / getpagesize ();
  return MAP_ISSET (off);
}

fhandler_base *
mmap_record::alloc_fh ()
{
  if (anonymous ())
    {
      fh_paging_file.set_io_handle (INVALID_HANDLE_VALUE);
      return &fh_paging_file;
    }

  /* The file descriptor could have been closed or, even
     worse, could have been reused for another file before
     the call to fork(). This requires creating a fhandler
     of the correct type to be sure to call the method of the
     correct class. */
  return build_fh_dev (get_device ());
}

void
mmap_record::free_fh (fhandler_base *fh)
{
  if (!anonymous ())
    cfree (fh);
}

mmap_record *
list::add_record (mmap_record r)
{
  if (nrecs == maxrecs)
    {
      mmap_record *new_recs;
      if (maxrecs == 0)
	new_recs = (mmap_record *)
			cmalloc (HEAP_MMAP, 5 * sizeof (mmap_record));
      else
	new_recs = (mmap_record *)
			crealloc (recs, (maxrecs + 5) * sizeof (mmap_record));
      if (!new_recs)
	return NULL;
      maxrecs += 5;
      recs = new_recs;
    }
  recs[nrecs] = r;
  if (!recs[nrecs].alloc_page_map ())
    return NULL;
  return recs + nrecs++;
}

/* Used in mmap() */
mmap_record *
list::search_record (_off64_t off, DWORD len)
{
  if (anonymous () && !off)
    {
      len = PAGE_CNT (len);
      for (int i = 0; i < nrecs; ++i)
	if (recs[i].find_unused_pages (len) != (DWORD)-1)
	  return recs + i;
    }
  else
    {
      for (int i = 0; i < nrecs; ++i)
	if (off >= recs[i].get_offset ()
	    && off + len <= recs[i].get_offset ()
			 + (PAGE_CNT (recs[i].get_len ()) * getpagesize ()))
	  return recs + i;
    }
  return NULL;
}

/* Used in munmap() */
long
list::search_record (caddr_t addr, DWORD len, caddr_t &m_addr, DWORD &m_len,
		     long start)
{
  caddr_t low, high;

  for (long i = start + 1; i < nrecs; ++i)
    {
      low = (addr >= recs[i].get_address ()) ? addr : recs[i].get_address ();
      high = recs[i].get_address ()
	     + (PAGE_CNT (recs[i].get_len ()) * getpagesize ());
      high = (addr + len < high) ? addr + len : high;
      if (low < high)
	{
	  m_addr = low;
	  m_len = high - low;
	  return i;
	}
    }
  return -1;
}

void
list::set (int nfd)
{
  fd = nfd;
  if (!anonymous ())
    hash = cygheap->fdtab[fd]->get_namehash ();
  nrecs = maxrecs = 0;
  recs = NULL;
}

bool
list::del_record (int i)
{
  if (i < nrecs)
    {
      recs[i].free_page_map ();
      for (; i < nrecs - 1; i++)
	recs[i] = recs[i + 1];
      nrecs--;
    }
  /* Return true if the list is empty which allows the caller to remove
     this list from the list array. */
  return !nrecs;
}

list *
map::get_list_by_fd (int fd)
{
  unsigned i;
  for (i = 0; i < nlists; i++)
    /* The fd isn't sufficient since it could already be the fd of another
       file.  So we use the name hash value to identify the file unless
       it's an anonymous mapping in which case the fd (-1) is sufficient. */
    if ((fd == -1 && lists[i].anonymous ())
	|| (fd != -1
	    && lists[i].get_hash () == cygheap->fdtab[fd]->get_namehash ()))
      return lists + i;
  return 0;
}

list *
map::add_list (int fd)
{
  if (nlists == maxlists)
    {
      list *new_lists;
      if (maxlists == 0)
	new_lists = (list *) cmalloc (HEAP_MMAP, 5 * sizeof (list));
      else
	new_lists = (list *) crealloc (lists, (maxlists + 5) * sizeof (list));
      if (!new_lists)
	return NULL;
      maxlists += 5;
      lists = new_lists;
    }
  lists[nlists].set (fd);
  return lists + nlists++;
}

void
map::del_list (unsigned i)
{
  if (i < nlists)
    {
      lists[i].free_recs ();
      for (; i < nlists - 1; i++)
	lists[i] = lists[i + 1];
      nlists--;
    }
}

extern "C" void *
mmap64 (void *addr, size_t len, int prot, int flags, int fd, _off64_t off)
{
  syscall_printf ("addr %x, len %u, prot %x, flags %x, fd %d, off %D",
		  addr, len, prot, flags, fd, off);

  caddr_t ret = (caddr_t) MAP_FAILED;
  fhandler_base *fh = NULL;
  mmap_record *rec;

  DWORD pagesize = getpagesize ();

  SetResourceLock (LOCK_MMAP_LIST, READ_LOCK | WRITE_LOCK, "mmap");

  /* Error conditions.  Note that the addr%pagesize test is deferred
     to workaround a serious alignment problem in Windows 98.  */
  if (off % pagesize
      || ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
      || ((flags & MAP_TYPE) != MAP_SHARED
	  && (flags & MAP_TYPE) != MAP_PRIVATE)
#if 0
      || (fixed (flags) && ((DWORD)addr % pagesize))
#endif
      || !len)
    {
      set_errno (EINVAL);
      goto out;
    }

  /* There's a serious alignment problem in Windows 98.  MapViewOfFile
     sometimes returns addresses which are page aligned instead of
     granularity aligned.  OTOH, it's not possible to force such an
     address using MapViewOfFileEx.  So what we do here to let it work
     at least most of the time is, allow 4K aligned addresses in 98,
     to enable remapping of formerly mapped pages.  If no matching
     free pages exist, check addr again, this time for the real alignment. */
  DWORD checkpagesize = wincap.has_mmap_alignment_bug () ?
  			getsystempagesize () : pagesize;
  if (fixed (flags) && ((DWORD) addr % checkpagesize))
    {
      set_errno (EINVAL);
      goto out;
    }

  if (anonymous (flags))
    fd = -1;
  else if (fd != -1)
    {
      /* Ensure that fd is open */
      cygheap_fdget cfd (fd);
      if (cfd < 0)
	goto out;

      /* Convert /dev/zero mapping to MAP_ANONYMOUS mapping. */
      fh = cfd;
      if (fh->get_device () == FH_ZERO)
	{
	  /* mmap /dev/zero is like MAP_ANONYMOUS. */
	  fd = -1;
	  flags |= MAP_ANONYMOUS;
	}
    }
  /* Don't use anonymous() here since that doesn't catch the fd == -1 case
     with no MAP_ANONYMOUS flags set. */
  if (fd == -1)
    {
      fh_paging_file.set_io_handle (INVALID_HANDLE_VALUE);
      fh = &fh_paging_file;
      /* Anonymous mappings are always forced to pagesize length. */
      len = PAGE_CNT (len) * pagesize;
      flags |= MAP_ANONYMOUS;
    }
  else if (fh->get_device () == FH_FS)
    {
      /* File mappings needs some extra care. */
      DWORD high;
      DWORD low = GetFileSize (fh->get_handle (), &high);
      _off64_t fsiz = ((_off64_t)high << 32) + low;

      /* Don't allow mappings beginning beyond EOF since Windows can't
	 handle that POSIX like, unless MAP_AUTOGROW flag is set, which
	 mimics Windows behaviour.  FIXME: Still looking for a good idea
	 to allow that under POSIX rules. */
      if (off >= fsiz && !autogrow (flags))
	{
	  set_errno (ENXIO);
	  goto out;
	}
      /* Don't map beyond EOF.  Windows would change the file to the
	 new length otherwise, in contrast to POSIX.  Allow mapping
	 beyond EOF if MAP_AUTOGROW flag is set. */
      fsiz -= off;
      if (len > fsiz)
	{
	  if (autogrow (flags))
	    {
	      /* Check if file has been opened for writing. */
	      if (!(fh->get_access () & GENERIC_WRITE))
		{
		  set_errno (EINVAL);
		  goto out;
		}
	    }
	  else
	    len = fsiz;
	}

      /* If the requested offset + len is <= file size, drop MAP_AUTOGROW.
	 This simplifes fhandler::mmap's job. */
      if (autogrow (flags) && (off + len) <= fsiz)
	flags &= ~MAP_AUTOGROW;
    }

  list *map_list = mmapped_areas.get_list_by_fd (fd);

  /* Test if an existing anonymous mapping can be recycled. */
  if (map_list && anonymous (flags))
    {
      if (off == 0 && !fixed (flags))
	{
	  /* If MAP_FIXED isn't given, check if this mapping matches into the
	     chunk of another already performed mapping. */
	  if ((rec = map_list->search_record (off, len)) != NULL
	      && rec->compatible_flags (flags))
	    {
	      if ((off = rec->map_pages (off, len)) == (_off64_t)-1)
		goto out;
	      ret = rec->get_address () + off;
	      goto out;
	    }
	}
      else if (fixed (flags))
	{
	  /* If MAP_FIXED is given, test if the requested area is in an
	     unmapped part of an still active mapping.  This can happen
	     if a memory region is unmapped and remapped with MAP_FIXED. */
	  caddr_t u_addr;
	  DWORD u_len;
	  long record_idx = -1;
	  if ((record_idx = map_list->search_record ((caddr_t)addr, len,
						     u_addr, u_len,
						     record_idx)) >= 0)
	    {
	      rec = map_list->get_record (record_idx);
	      if (u_addr > (caddr_t)addr || u_addr + len < (caddr_t)addr + len
		  || !rec->compatible_flags (flags))
		{
		  /* Partial match only, or access mode doesn't match. */
		  /* FIXME: Handle partial mappings gracefully if adjacent
		     memory is available. */
		  set_errno (EINVAL);
		  goto out;
		}
	      if (!rec->map_pages ((caddr_t)addr, len))
		goto out;
	      ret = (caddr_t)addr;
	      goto out;
	    }
	}
    }

  /* Deferred alignment test, see above. */
  if (wincap.has_mmap_alignment_bug ()
      && fixed (flags) && ((DWORD) addr % pagesize))
    {
      set_errno (EINVAL);
      goto out;
    }

  caddr_t base = (caddr_t)addr;
  HANDLE h = fh->mmap (&base, len, prot, flags, off);
  if (h == INVALID_HANDLE_VALUE)
    goto out;

  /* At this point we should have a successfully mmapped area.
     Now it's time for bookkeeping stuff. */

  /* Get list of mmapped areas for this fd, create a new one if
     one does not exist yet.  */
  if (!map_list && !(map_list = mmapped_areas.add_list (fd)))
    {
      fh->munmap (h, base, len);
      set_errno (ENOMEM);
      goto out;
    }

  /* Insert into the list */
  {
    mmap_record mmap_rec (fd, h, prot, flags, off, len, base);
    rec = map_list->add_record (mmap_rec);
  }
  if (!rec)
    {
      fh->munmap (h, base, len);
      set_errno (ENOMEM);
      goto out;
    }

  ret = base;

out:

  ReleaseResourceLock (LOCK_MMAP_LIST, READ_LOCK | WRITE_LOCK, "mmap");
  syscall_printf ("%p = mmap() ", ret);
  return ret;
}

extern "C" void *
mmap (void *addr, size_t len, int prot, int flags, int fd, _off_t off)
{
  return mmap64 (addr, len, prot, flags, fd, (_off64_t)off);
}

/* munmap () removes all mmapped pages between addr and addr+len. */

extern "C" int
munmap (void *addr, size_t len)
{
  syscall_printf ("munmap (addr %x, len %u)", addr, len);

  /* Error conditions according to SUSv3 */
  if (!addr || !len || check_invalid_virtual_addr (addr, len))
    {
      set_errno (EINVAL);
      return -1;
    }
  /* See comment in mmap64 for a description. */
  DWORD checkpagesize = wincap.has_mmap_alignment_bug () ?
  			getsystempagesize () : getpagesize ();
  if (((DWORD) addr % checkpagesize) || !len)
    {
      set_errno (EINVAL);
      return -1;
    }

  SetResourceLock (LOCK_MMAP_LIST, WRITE_LOCK | READ_LOCK, "munmap");

  /* Iterate through the map, unmap pages between addr and addr+len
     in all maps. */
  list *map_list;
  for (unsigned list_idx = 0;
       (map_list = mmapped_areas.get_list (list_idx));
       ++list_idx)
    {
      long record_idx = -1;
      caddr_t u_addr;
      DWORD u_len;

      while ((record_idx = map_list->search_record((caddr_t)addr, len, u_addr,
						   u_len, record_idx)) >= 0)
	{
	  mmap_record *rec = map_list->get_record (record_idx);
	  if (rec->unmap_pages (u_addr, u_len))
	    {
	      /* The whole record has been unmapped, so we now actually
		 unmap it from the system in full length... */
	      fhandler_base *fh = rec->alloc_fh ();
	      fh->munmap (rec->get_handle (),
			  rec->get_address (),
			  rec->get_len ());
	      rec->free_fh (fh);

	      /* ...and delete the record. */
	      if (map_list->del_record (record_idx--))
		{
		  /* Yay, the last record has been removed from the list,
		     we can remove the list now, too. */
		  mmapped_areas.del_list (list_idx--);
		  break;
		}
	    }
	}
    }

  ReleaseResourceLock (LOCK_MMAP_LIST, WRITE_LOCK | READ_LOCK, "munmap");
  syscall_printf ("0 = munmap(): %x", addr);
  return 0;
}

/* Sync file with memory. Ignore flags for now. */

extern "C" int
msync (void *addr, size_t len, int flags)
{
  syscall_printf ("addr %x, len %u, flags %x", addr, len, flags);

  int ret = -1;
  list *map_list;

  SetResourceLock (LOCK_MMAP_LIST, WRITE_LOCK | READ_LOCK, "msync");

  /* However, check flags for validity. */
  if ((flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE))
      || ((flags & MS_ASYNC) && (flags & MS_SYNC)))
    {
      set_errno (EINVAL);
      goto out;
    }

  /* Iterate through the map, looking for the mmapped area.
     Error if not found. */
  for (unsigned list_idx = 0;
       (map_list = mmapped_areas.get_list (list_idx));
       ++list_idx)
    {
      mmap_record *rec;
      for (int record_idx = 0;
	   (rec = map_list->get_record (record_idx));
	   ++record_idx)
	{
	  if (rec->access ((caddr_t)addr))
	    {
	      /* Check whole area given by len. */
	      for (DWORD i = getpagesize (); i < len; ++i)
		if (!rec->access ((caddr_t)addr + i))
		  {
		    set_errno (ENOMEM);
		    goto out;
		  }
	      fhandler_base *fh = rec->alloc_fh ();
	      ret = fh->msync (rec->get_handle (), (caddr_t)addr, len, flags);
	      rec->free_fh (fh);
	      goto out;
	    }
	}
    }

  /* No matching mapping exists. */
  set_errno (ENOMEM);

out:
  syscall_printf ("%d = msync()", ret);
  ReleaseResourceLock (LOCK_MMAP_LIST, WRITE_LOCK | READ_LOCK, "msync");
  return ret;
}

/* Set memory protection */

extern "C" int
mprotect (void *addr, size_t len, int prot)
{
  DWORD old_prot;
  DWORD new_prot = 0;

  syscall_printf ("mprotect (addr %x, len %u, prot %x)", addr, len, prot);

  bool in_mapped = false;
  bool ret = false;

  SetResourceLock (LOCK_MMAP_LIST, WRITE_LOCK | READ_LOCK, "mprotect");
 
  /* Iterate through the map, protect pages between addr and addr+len
     in all maps. */
  list *map_list;
  for (unsigned list_idx = 0;
      (map_list = mmapped_areas.get_list (list_idx));
      ++list_idx)
   {
     long record_idx = -1;
     caddr_t u_addr;
     DWORD u_len;

     while ((record_idx = map_list->search_record((caddr_t)addr, len,
						  u_addr, u_len,
						  record_idx)) >= 0)
       {
	 mmap_record *rec = map_list->get_record (record_idx);
	 in_mapped = true;
	 new_prot = gen_protect (prot, rec->get_flags ());
	 if (rec->anonymous () && rec->priv () && rec->noreserve ())
	   {
	     if (new_prot == PAGE_NOACCESS)
	       ret = VirtualFree (u_addr, u_len, MEM_DECOMMIT);
	     else
	       ret = !!VirtualAlloc (u_addr, u_len, MEM_COMMIT, new_prot);
	   }
	 else
	   ret = VirtualProtect (u_addr, u_len, new_prot, &old_prot);
	 if (!ret)
	   {
	     ReleaseResourceLock (LOCK_MMAP_LIST, WRITE_LOCK | READ_LOCK,
				  "mprotect");
	     __seterrno ();
	     syscall_printf ("-1 = mprotect (), %E");
	     return -1;
	   }
       }
   }

  ReleaseResourceLock (LOCK_MMAP_LIST, WRITE_LOCK | READ_LOCK, "mprotect");

  if (!in_mapped)
    {
      int flags = 0;
      MEMORY_BASIC_INFORMATION mbi;

      if (!VirtualQuery (addr, &mbi, sizeof mbi))
        {
	  __seterrno ();
	  syscall_printf ("-1 = mprotect (), %E");
	  return -1;
	}

      /* If write protection is requested, check if the page was
	 originally protected writecopy.  In this case call VirtualProtect
	 requesting PAGE_WRITECOPY, otherwise the VirtualProtect will fail
	 on NT version >= 5.0 */
      if (prot & PROT_WRITE)
	{
	  if (mbi.AllocationProtect == PAGE_WRITECOPY
	      || mbi.AllocationProtect == PAGE_EXECUTE_WRITECOPY)
	    flags = MAP_PRIVATE;
	}
      new_prot = gen_protect (prot, flags);
      if (new_prot != PAGE_NOACCESS && mbi.State == MEM_RESERVE)
	ret = VirtualAlloc (addr, len, MEM_COMMIT, new_prot);
      else
	ret = VirtualProtect (addr, len, new_prot, &old_prot);
      if (!ret)
	{
	  __seterrno ();
	  syscall_printf ("-1 = mprotect (), %E");
	  return -1;
	}
    }

  syscall_printf ("0 = mprotect ()");
  return 0;
}

extern "C" int
mlock (const void *addr, size_t len)
{
  if (!wincap.has_working_virtual_lock ())
    return 0;

  int ret = -1;

  /* Note that we're using getpagesize, not getsystempagesize.  This way, the
     alignment matches the notion the application has of the page size. */
  size_t pagesize = getpagesize ();

  /* Instead of using VirtualLock, which does not guarantee that the pages
     aren't swapped out when the process is inactive, we're using
     ZwLockVirtualMemory with the LOCK_VM_IN_RAM flag to do what mlock on
     POSIX systems does.  On NT, this requires SeLockMemoryPrivilege,
     which is given only to SYSTEM by default. */

  push_thread_privilege (SE_LOCK_MEMORY_PRIV, true);

  /* Align address and length values to page size. */
  PVOID base = (PVOID) ((uintptr_t) addr & ~(pagesize - 1));
  ULONG size = ((uintptr_t) addr - (uintptr_t) base) + len;
  size = (size + pagesize - 1) & ~(pagesize - 1);
  NTSTATUS status = 0;
  do
    {
      status = NtLockVirtualMemory (hMainProc, &base, &size, LOCK_VM_IN_RAM);
      if (status == STATUS_WORKING_SET_QUOTA)
	{
	  /* The working set is too small, try to increase it so that the
	     requested locking region fits in.  Unfortunately I don't know
	     any function which would return the currently locked pages of
	     a process (no go with NtQueryVirtualMemory).
	     
	     So, except for the border cases, what we do here is something
	     really embarrassing.  We raise the working set by 64K at a time
	     and retry, until either we fail to raise the working set size
	     further, or until NtLockVirtualMemory returns successfully (or
	     with another error).  */
	  ULONG min, max;
	  if (!GetProcessWorkingSetSize (hMainProc, &min, &max))
	    {
	      set_errno (ENOMEM);
	      break;
	    }
	  if (min < size)
	    min = size + pagesize;
	  else if (size < pagesize)
	    min += size;
	  else
	    min += pagesize;
	  if (max < min)
	    max = min;
	  if (!SetProcessWorkingSetSize (hMainProc, min, max))
	    {
	      set_errno (ENOMEM);
	      break;
	    }
	}
      else if (!NT_SUCCESS (status))
	__seterrno_from_nt_status (status);
      else
        ret = 0;
    }
  while (status == STATUS_WORKING_SET_QUOTA);

  pop_thread_privilege ();

  return ret;
}

extern "C" int
munlock (const void *addr, size_t len)
{
  if (!wincap.has_working_virtual_lock ())
    return 0;

  int ret = -1;

  push_thread_privilege (SE_LOCK_MEMORY_PRIV, true);

  PVOID base = (PVOID) addr;
  ULONG size = len;
  NTSTATUS status = NtUnlockVirtualMemory (hMainProc, &base, &size,
					   LOCK_VM_IN_RAM);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  else
    ret = 0;

  pop_thread_privilege ();

  return ret;
}

/*
 * Base implementation:
 *
 * `mmap' returns ENODEV as documented in SUSv2.
 * In contrast to the global function implementation, the member function
 * `mmap' has to return the mapped base address in `addr' and the handle to
 * the mapping object as return value. In case of failure, the fhandler
 * mmap has to close that handle by itself and return INVALID_HANDLE_VALUE.
 *
 * `munmap' and `msync' get the handle to the mapping object as first parameter
 * additionally.
*/
HANDLE
fhandler_base::mmap (caddr_t *addr, size_t len, int prot,
		     int flags, _off64_t off)
{
  set_errno (ENODEV);
  return INVALID_HANDLE_VALUE;
}

int
fhandler_base::munmap (HANDLE h, caddr_t addr, size_t len)
{
  set_errno (ENODEV);
  return -1;
}

int
fhandler_base::msync (HANDLE h, caddr_t addr, size_t len, int flags)
{
  set_errno (ENODEV);
  return -1;
}

bool
fhandler_base::fixup_mmap_after_fork (HANDLE h, int prot, int flags,
				      _off64_t offset, DWORD size,
				      void *address)
{
  set_errno (ENODEV);
  return -1;
}

/* Implementation for anonymous maps.  Using fhandler_dev_zero looks
   quite the natural way. */
HANDLE
fhandler_dev_zero::mmap (caddr_t *addr, size_t len, int prot,
			 int flags, _off64_t off)
{
  HANDLE h;
  void *base;

  if (priv (flags))
    {
      /* Private anonymous maps are now implemented using VirtualAlloc.
         This has two advantages:

	 - VirtualAlloc has a smaller footprint than a copy-on-write
	   anonymous map.

	 - It supports decommitting using VirtualFree, in contrast to
	   section maps.  This allows minimum footprint private maps,
	   when using the (non-POSIX, yay-Linux) MAP_NORESERVE flag.
      */
      DWORD protect = gen_protect (prot, flags);
      DWORD alloc_type = MEM_RESERVE | (noreserve (flags) ? 0 : MEM_COMMIT);
      base = VirtualAlloc (*addr, len, alloc_type, protect);
      if (!base && addr && !fixed (flags))
	base = VirtualAlloc (NULL, len, alloc_type, protect);
      if (!base || (fixed (flags) && base != *addr))
	{
	  if (!base)
	    __seterrno ();
	  else
	    {
	      VirtualFree (base, len, MEM_RELEASE);
	      set_errno (EINVAL);
	      syscall_printf ("VirtualAlloc: address shift with MAP_FIXED given");
	    }
	  return INVALID_HANDLE_VALUE;
	}
      h = (HANDLE) 1; /* Fake handle to indicate success. */
    }
  else
    {
      h = mmap_func->CreateMapping (get_handle (), len, off, prot, flags,
				    get_name ());
      if (!h)
	{
	  __seterrno ();
	  syscall_printf ("CreateMapping failed with %E");
	  return INVALID_HANDLE_VALUE;
	}

      base = mmap_func->MapView (h, *addr, len, prot, flags, off);
      if (!base || (fixed (flags) && base != *addr))
	{
	  if (!base)
	    __seterrno ();
	  else
	    {
	      UnmapViewOfFile (base);
	      set_errno (EINVAL);
	      syscall_printf ("MapView: address shift with MAP_FIXED given");
	    }
	  CloseHandle (h);
	  return INVALID_HANDLE_VALUE;
	}

    }
  *addr = (caddr_t) base;
  return h;
}

int
fhandler_dev_zero::munmap (HANDLE h, caddr_t addr, size_t len)
{
  VirtualFree (addr, len, MEM_RELEASE);
  return 0;
}

int
fhandler_dev_zero::msync (HANDLE h, caddr_t addr, size_t len, int flags)
{
  return 0;
}

bool
fhandler_dev_zero::fixup_mmap_after_fork (HANDLE h, int prot, int flags,
				      _off64_t offset, DWORD size,
				      void *address)
{
  /* Re-create the map */
  void *base;
  if (priv (flags))
    {
      DWORD protect = gen_protect (prot, flags);
      DWORD alloc_type = MEM_RESERVE | (noreserve (flags) ? 0 : MEM_COMMIT);
      base = VirtualAlloc (address, size, alloc_type, protect);
    }
  else
    base = mmap_func->MapView (h, address, size, prot, flags, offset);
  if (base != address)
    {
      MEMORY_BASIC_INFORMATION m;
      VirtualQuery (address, &m, sizeof (m));
      system_printf ("requested %p != %p mem alloc base %p, state %p, "
		     "size %d, %E", address, base, m.AllocationBase, m.State,
		     m.RegionSize);
    }
  return base == address;
}

/* Implementation for disk files and anonymous mappings. */
HANDLE
fhandler_disk_file::mmap (caddr_t *addr, size_t len, int prot,
			  int flags, _off64_t off)
{
  HANDLE h = mmap_func->CreateMapping (get_handle (), len, off, prot, flags,
				       get_name ());
  if (!h)
    {
      __seterrno ();
      syscall_printf ("CreateMapping failed with %E");
      return INVALID_HANDLE_VALUE;
    }

  void *base = mmap_func->MapView (h, *addr, len, prot, flags, off);
  if (!base || (fixed (flags) && base != *addr))
    {
      if (!base)
	__seterrno ();
      else
	{
	  UnmapViewOfFile (base);
	  set_errno (EINVAL);
	  syscall_printf ("MapView: address shift with MAP_FIXED given");
	}
      CloseHandle (h);
      return INVALID_HANDLE_VALUE;
    }

  *addr = (caddr_t) base;
  return h;
}

int
fhandler_disk_file::munmap (HANDLE h, caddr_t addr, size_t len)
{
  UnmapViewOfFile (addr);
  CloseHandle (h);
  return 0;
}

int
fhandler_disk_file::msync (HANDLE h, caddr_t addr, size_t len, int flags)
{
  if (FlushViewOfFile (addr, len) == 0)
    {
      __seterrno ();
      return -1;
    }
  return 0;
}

bool
fhandler_disk_file::fixup_mmap_after_fork (HANDLE h, int prot, int flags,
					   _off64_t offset, DWORD size,
					   void *address)
{
  /* Re-create the map */
  void *base = mmap_func->MapView (h, address, size, prot, flags, offset);
  if (base != address)
    {
      MEMORY_BASIC_INFORMATION m;
      VirtualQuery (address, &m, sizeof (m));
      system_printf ("requested %p != %p mem alloc base %p, state %p, "
		     "size %d, %E", address, base, m.AllocationBase, m.State,
		     m.RegionSize);
    }
  return base == address;
}

HANDLE
fhandler_dev_mem::mmap (caddr_t *addr, size_t len, int prot,
			int flags, _off64_t off)
{
  if (off >= mem_size
      || (DWORD) len >= mem_size
      || off + len >= mem_size)
    {
      set_errno (EINVAL);
      syscall_printf ("-1 = mmap(): illegal parameter, set EINVAL");
      return INVALID_HANDLE_VALUE;
    }

  UNICODE_STRING memstr;
  RtlInitUnicodeString (&memstr, L"\\device\\physicalmemory");

  OBJECT_ATTRIBUTES attr;
  InitializeObjectAttributes (&attr, &memstr,
			      OBJ_CASE_INSENSITIVE | OBJ_INHERIT,
			      NULL, NULL);

  /* Section access is bit-wise ored, while on the Win32 level access
     is only one of the values.  It's not quite clear if the section
     access has to be defined this way, or if SECTION_ALL_ACCESS would
     be sufficient but this worked fine so far, so why change? */
  ACCESS_MASK section_access;
  if (prot & PROT_WRITE)
    section_access = SECTION_MAP_READ | SECTION_MAP_WRITE;
  else
    section_access = SECTION_MAP_READ;

  HANDLE h;
  NTSTATUS ret = NtOpenSection (&h, section_access, &attr);
  if (!NT_SUCCESS (ret))
    {
      __seterrno_from_nt_status (ret);
      syscall_printf ("-1 = mmap(): NtOpenSection failed with %E");
      return INVALID_HANDLE_VALUE;
    }

  void *base = MapViewNT (h, *addr, len, prot, flags | MAP_ANONYMOUS, off);
  if (!base || (fixed (flags) && base != *addr))
    {
      if (!base)
        __seterrno ();
      else
        {
	  NtUnmapViewOfSection (GetCurrentProcess (), base);
	  set_errno (EINVAL);
	  syscall_printf ("MapView: address shift with MAP_FIXED given");
	}
      CloseHandle (h);
      return INVALID_HANDLE_VALUE;
    }

  *addr = (caddr_t) base;
  return h;
}

int
fhandler_dev_mem::munmap (HANDLE h, caddr_t addr, size_t len)
{
  NTSTATUS ret;
  if (!NT_SUCCESS (ret = NtUnmapViewOfSection (INVALID_HANDLE_VALUE, addr)))
    {
      __seterrno_from_nt_status (ret);
      return -1;
    }
  CloseHandle (h);
  return 0;
}

int
fhandler_dev_mem::msync (HANDLE h, caddr_t addr, size_t len, int flags)
{
  return 0;
}

bool
fhandler_dev_mem::fixup_mmap_after_fork (HANDLE h, int prot, int flags,
					 _off64_t offset, DWORD size,
					 void *address)
{
  void *base = MapViewNT (h, address, size, prot, flags | MAP_ANONYMOUS, offset);
  if (base != address)
    {
      MEMORY_BASIC_INFORMATION m;
      VirtualQuery (address, &m, sizeof (m));
      system_printf ("requested %p != %p mem alloc base %p, state %p, "
		     "size %d, %E", address, base, m.AllocationBase, m.State,
		     m.RegionSize);
    }
  return base == address;
}

/* Call to re-create all the file mappings in a forked child. Called from
   the child in initialization. At this point we are passed a valid
   mmapped_areas map, and all the HANDLE's are valid for the child, but
   none of the mapped areas are in our address space. We need to iterate
   through the map, doing the MapViewOfFile calls.  */

int __stdcall
fixup_mmaps_after_fork (HANDLE parent)
{
  /* Iterate through the map */
  list *map_list;
  for (unsigned list_idx = 0;
       (map_list = mmapped_areas.get_list (list_idx));
       ++list_idx)
    {
      mmap_record *rec;
      for (int record_idx = 0;
	   (rec = map_list->get_record (record_idx));
	   ++record_idx)
	{
	  debug_printf ("fd %d, h %x, access %x, offset %D, size %u, "
	  		"address %p", rec->get_fd (), rec->get_handle (),
			rec->gen_access (), rec->get_offset (),
			rec->get_len (), rec->get_address ());

	  fhandler_base *fh = rec->alloc_fh ();
	  bool ret = fh->fixup_mmap_after_fork (rec->get_handle (),
						rec->get_prot (),
						rec->get_flags (),
						rec->get_offset (),
						rec->get_len (),
						rec->get_address ());
	  rec->free_fh (fh);

	  if (!ret)
	    return -1;

	  MEMORY_BASIC_INFORMATION mbi;
	  DWORD old_prot;

	  for (char *address = rec->get_address ();
	       address < rec->get_address () + rec->get_len ();
	       address += mbi.RegionSize)
	    {
	      if (!VirtualQueryEx (parent, address, &mbi, sizeof mbi))
	        {
		  system_printf ("VirtualQueryEx failed for MAP_PRIVATE "
		  		 "address %p, %E", address);
		  return -1;
		}
	      /* Set reserved pages to reserved in child. */
	      if (mbi.State == MEM_RESERVE)
	        {
		  VirtualFree (address, mbi.RegionSize, MEM_DECOMMIT);
		  continue;
		}
	      /* Copy-on-write pages must be copied to the child to circumvent
	         a strange notion how copy-on-write is supposed to work. */
	      if (rec->priv ())
		{
		  if (rec->anonymous () && rec->noreserve ()
		      && !VirtualAlloc (address, mbi.RegionSize,
		      			MEM_COMMIT, PAGE_READWRITE))
		    {
		      system_printf ("VirtualAlloc failed for MAP_PRIVATE "
				     "address %p, %E", address);
		      return -1;
		    }
		  if (mbi.Protect == PAGE_NOACCESS
		      && !VirtualProtectEx (parent, address, mbi.RegionSize,
					    PAGE_READONLY, &old_prot))
		    {
		      system_printf ("VirtualProtectEx failed for MAP_PRIVATE "
				     "address %p, %E", address);
		      return -1;
		    }
		  else if (!rec->anonymous ()
			   && (mbi.Protect == PAGE_READWRITE
			       || mbi.Protect == PAGE_EXECUTE_READWRITE))
		    {
		      /* A PAGE_WRITECOPY page which has been written to is 
			 set to PAGE_READWRITE, but that's an incompatible
			 protection to set the page to. */
		      mbi.Protect &= ~PAGE_READWRITE;
		      mbi.Protect |= PAGE_WRITECOPY;
		    }
		  if (!ReadProcessMemory (parent, address, address,
					  mbi.RegionSize, NULL))
		    {
		      system_printf ("ReadProcessMemory failed for MAP_PRIVATE "
				     "address %p, %E", address);
		      return -1;
		    }
		  if (mbi.Protect == PAGE_NOACCESS
		      && !VirtualProtectEx (parent, address, mbi.RegionSize,
					    PAGE_NOACCESS, &old_prot))
		    {
		      system_printf ("WARNING: VirtualProtectEx to return to "
				     "PAGE_NOACCESS state in parent failed for "
				     "MAP_PRIVATE address %p, %E", address);
		      return -1;
		    }
		}
	      /* Set child page protection to parent protection if
	         protection differs from original protection. */
	      if (!VirtualProtect (address, mbi.RegionSize,
				   mbi.Protect, &old_prot))
		{
		  MEMORY_BASIC_INFORMATION m;
		  VirtualQuery (address, &m, sizeof m);
		  system_printf ("VirtualProtect failed for "
		  		 "address %p, "
				 "parentstate: 0x%x, "
				 "state: 0x%x, "
				 "parentprot: 0x%x, "
				 "prot: 0x%x, %E",
				 address, mbi.State, m.State,
				 mbi.Protect, m.Protect);
		  return -1;
		}
	    }
	}
    }

  debug_printf ("succeeded");
  return 0;
}
