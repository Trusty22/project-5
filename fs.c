// ============================================================================
// fs.c - user FileSytem API
// ============================================================================

#include "fs.h"
#include "bfs.h"

// ============================================================================
// Close the file currently open on file descriptor 'fd'.
// ============================================================================
i32 fsClose(i32 fd) {
  i32 inum = bfsFdToInum(fd);
  bfsDerefOFT(inum);
  return 0;
}

// ============================================================================
// Create the file called 'fname'.  Overwrite, if it already exsists.
// On success, return its file descriptor.  On failure, EFNF
// ============================================================================
i32 fsCreate(str fname) {
  i32 inum = bfsCreateFile(fname);
  if (inum == EFNF)
    return EFNF;
  return bfsInumToFd(inum);
}

// ============================================================================
// Format the BFS disk by initializing the SuperBlock, Inodes, Directory and
// Freelist.  On succes, return 0.  On failure, abort
// ============================================================================
i32 fsFormat() {
  FILE *fp = fopen(BFSDISK, "w+b");
  if (fp == NULL)
    FATAL(EDISKCREATE);

  i32 ret = bfsInitSuper(fp); // initialize Super block
  if (ret != 0) {
    fclose(fp);
    FATAL(ret);
  }

  ret = bfsInitInodes(fp); // initialize Inodes block
  if (ret != 0) {
    fclose(fp);
    FATAL(ret);
  }

  ret = bfsInitDir(fp); // initialize Dir block
  if (ret != 0) {
    fclose(fp);
    FATAL(ret);
  }

  ret = bfsInitFreeList(); // initialize Freelist
  if (ret != 0) {
    fclose(fp);
    FATAL(ret);
  }

  ret = bfsInitOFT(); // initialize OFT
  if (ret != 0) {
    fclose(fp);
    FATAL(ret);
  }

  fclose(fp);
  return 0;
}

// ============================================================================
// Mount the BFS disk.  It must already exist
// ============================================================================
i32 fsMount() {
  FILE *fp = fopen(BFSDISK, "rb");
  if (fp == NULL)
    FATAL(ENODISK); // BFSDISK not found
  fclose(fp);
  return 0;
}

// ============================================================================
// Open the existing file called 'fname'.  On success, return its file
// descriptor.  On failure, return EFNF
// ============================================================================
i32 fsOpen(str fname) {
  i32 inum = bfsLookupFile(fname); // lookup 'fname' in Directory
  if (inum == EFNF)
    return EFNF;
  return bfsInumToFd(inum);
}

// ============================================================================
// Read 'numb' bytes of data from the cursor in the file currently fsOpen'd on
// File Descriptor 'fd' into 'buf'.  On success, return actual number of bytes
// read (may be less than 'numb' if we hit EOF).  On failure, abort
// ============================================================================
i32 fsRead(i32 fd, i32 numb, void *buf) {

  i32 inum = bfsFdToInum(fd);
  i32 ptr = bfsTell(fd);
  i32 currBlk = ptr / BYTESPERBLOCK;

  i32 length = numb;
  i32 size = 0;
  i32 tmptr;

  i8 buffer[BUFSIZ];

  if (bfsGetSize(inum) < ptr + numb) {
    length = bfsGetSize(inum) - ptr;
  }

  do {
    if (length < BYTESPERBLOCK) {
      numb = length;
      length = 0;
    } else {
      numb = BYTESPERBLOCK;
      length -= BYTESPERBLOCK;
    }

    bfsRead(inum, currBlk, buffer);
    memmove(buf + size, buffer, numb);
    memset(buffer, 0, BYTESPERBLOCK);

    tmptr = ptr += numb;
    bfsSetCursor(inum, tmptr);

    size = size + numb;
    currBlk++;

  } while (length > 0);

  return size;
}

// ============================================================================
// Move the cursor for the file currently open on File Descriptor 'fd' to the
// byte-offset 'offset'.  'whence' can be any of:
//
//  SEEK_SET : set cursor to 'offset'
//  SEEK_CUR : add 'offset' to the current cursor
//  SEEK_END : add 'offset' to the size of the file
//
// On success, return 0.  On failure, abort
// ============================================================================
i32 fsSeek(i32 fd, i32 offset, i32 whence) {

  if (offset < 0)
    FATAL(EBADCURS);

  i32 inum = bfsFdToInum(fd);
  i32 ofte = bfsFindOFTE(inum);

  switch (whence) {
  case SEEK_SET:
    g_oft[ofte].curs = offset;
    break;
  case SEEK_CUR:
    g_oft[ofte].curs += offset;
    break;
  case SEEK_END: {
    i32 end = fsSize(fd);
    g_oft[ofte].curs = end + offset;
    break;
  }
  default:
    FATAL(EBADWHENCE);
  }
  return 0;
}

// ============================================================================
// Return the cursor position for the file open on File Descriptor 'fd'
// ============================================================================
i32 fsTell(i32 fd) {
  return bfsTell(fd);
}

// ============================================================================
// Retrieve the current file size in bytes.  This depends on the highest offset
// written to the file, or the highest offset set with the fsSeek function.  On
// success, return the file size.  On failure, abort
// ============================================================================
i32 fsSize(i32 fd) {
  i32 inum = bfsFdToInum(fd);
  return bfsGetSize(inum);
}

// ============================================================================
// Write 'numb' bytes of data from 'buf' into the file currently fsOpen'd on
// filedescriptor 'fd'.  The write starts at the current file offset for the
// destination file.  On success, return 0.  On failure, abort
// ============================================================================
i32 fsWrite(i32 fd, i32 numb, void *buf) {
  i32 inum = bfsFdToInum(fd);
  i32 ptr = bfsTell(fd); // Gets the current pointer for our curser

  i32 ptrF = ptr / BYTESPERBLOCK; // Position of the pointer of the fbn
  i32 startFbn = ptr / BYTESPERBLOCK;
  i32 endFbn = (numb + ptr) / BYTESPERBLOCK;
  i32 size = (endFbn - startFbn + 1) * BYTESPERBLOCK; // Total size of the buffer
  i32 offset = ptr % BYTESPERBLOCK;                   // Get the offset
  i32 shift = 0;                                      // Buffer shift value

  // Buffers for the start and end blocks. as well as the combined data.
  i8 startB[BYTESPERBLOCK];
  i8 endB[BYTESPERBLOCK];
  i8 buffer[size];

  // Read start block of the file into the start buffer.
  bfsRead(inum, ptr / BYTESPERBLOCK, startB);

  // If the file is > last block, read the last block.
  if (bfsGetSize(inum) > (endFbn * BYTESPERBLOCK)) {
    bfsRead(inum, endFbn, endB);
  }
  memmove(buffer, startB, offset); // Copy from the start buffer into the main buffer .

  // Copy from end buffer into the main buffer if buffer is to big.
  if (bfsGetSize(inum) > (endFbn * BYTESPERBLOCK)) {
    memmove(buffer + (endFbn - startFbn) * BYTESPERBLOCK, endB, BYTESPERBLOCK);
  }
  memmove(buffer + offset, buf, numb); // Move numb from buf to the buffer + offset

  // If the block doesnt exist, extend file, this is done for the whole file.
  while (size > 0) {

    if ((bfsGetSize(inum) - 1) < (ptrF * BYTESPERBLOCK)) {
      bfsExtend(inum, ptrF);
    }
    bioWrite(bfsFbnToDbn(inum, ptrF), (buffer + shift));

    // adjusting size, buffer shift, and block ptr.
    size -= BYTESPERBLOCK;
    shift += BYTESPERBLOCK;
    ptrF++;
  }

  // New Cursor position is set after new offset is calculated.
  bfsSetCursor(inum, (numb + ptr));

  // Incase file size is still to big make it the same as the last file.
  if (bfsGetSize(inum) < (numb + ptr)) {
    bfsSetSize(inum, (numb + ptr));
  }

  return 0;
}
