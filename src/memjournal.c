/*
** 2008 October 7
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains code use to implement an in-memory rollback journal.
** The in-memory rollback journal is used to journal transactions for
** ":memory:" databases and when the journal_mode=MEMORY pragma is used.
*/
#include "sqliteInt.h"

/* Forward references to internal structures */
typedef struct MemJournal MemJournal;
typedef struct FilePoint FilePoint;
typedef struct FileChunk FileChunk;

/*
** The rollback journal is composed of a linked list of these structures.
**
** The zChunk array is always at least 8 bytes in size - usually much more.
** Its actual size is stored in the MemJournal.nChunkSize variable.
*/
struct FileChunk {
  FileChunk *pNext;               /* Next chunk in the journal */
  u8 zChunk[8];                   /* Content of this chunk */
};

/*
** By default, allocate this many bytes of memory for each FileChunk object.
*/
#define MEMJOURNAL_DFLT_FILECHUNKSIZE 1024

/*
** For chunk size nChunkSize, return the number of bytes that should
** be allocated for each FileChunk structure.
*/
#define fileChunkSize(nChunkSize) (sizeof(FileChunk) + ((nChunkSize)-8))

/*
** An instance of this object serves as a cursor into the rollback journal.
** The cursor can be either for reading or writing.
*/
struct FilePoint {
  sqlite3_int64 iOffset;          /* Offset from the beginning of the file */
  FileChunk *pChunk;              /* Specific chunk into which cursor points */
};

/*
** This structure is a subclass of sqlite3_file. Each open memory-journal
** is an instance of this class.
*/
struct MemJournal {
  const sqlite3_io_methods *pMethod; /* Parent class. MUST BE FIRST */
  int nChunkSize;                 /* In-memory chunk-size */

  int nBuf;                       /* Bytes of data before flushing */
  int nSize;                      /* Bytes of data currently in memory */
  FileChunk *pFirst;              /* Head of in-memory chunk-list */
  FilePoint endpoint;             /* Pointer to the end of the file */
  FilePoint readpoint;            /* Pointer to the end of the last xRead() */

  int flags;                      /* xOpen flags */
  sqlite3_vfs *pVfs;              /* The "real" underlying VFS */
  const char *zJournal;           /* Name of the journal file */
  sqlite3_file *pReal;            /* The "real" underlying file descriptor */
};

/*
** Read data from the in-memory journal file.  This is the implementation
** of the sqlite3_vfs.xRead method.
*/
static int memjrnlRead(
  sqlite3_file *pJfd,    /* The journal file from which to read */
  void *zBuf,            /* Put the results here */
  int iAmt,              /* Number of bytes to read */
  sqlite_int64 iOfst     /* Begin reading at this offset */
){
  MemJournal *p = (MemJournal *)pJfd;
  if( p->pReal ){
    return sqlite3OsRead(p->pReal, zBuf, iAmt, iOfst);
  }else if( (iAmt+iOfst)>p->endpoint.iOffset ){
    return SQLITE_IOERR_SHORT_READ;
  }else{
    u8 *zOut = zBuf;
    int nRead = iAmt;
    int iChunkOffset;
    FileChunk *pChunk;

    if( p->readpoint.iOffset!=iOfst || iOfst==0 ){
      sqlite3_int64 iOff = 0;
      for(pChunk=p->pFirst; 
          ALWAYS(pChunk) && (iOff+p->nChunkSize)<=iOfst;
          pChunk=pChunk->pNext
      ){
        iOff += p->nChunkSize;
      }
    }else{
      pChunk = p->readpoint.pChunk;
    }

    iChunkOffset = (int)(iOfst%p->nChunkSize);
    do {
      int iSpace = p->nChunkSize - iChunkOffset;
      int nCopy = MIN(nRead, (p->nChunkSize - iChunkOffset));
      memcpy(zOut, &pChunk->zChunk[iChunkOffset], nCopy);
      zOut += nCopy;
      nRead -= iSpace;
      iChunkOffset = 0;
    } while( nRead>=0 && (pChunk=pChunk->pNext)!=0 && nRead>0 );
    p->readpoint.iOffset = iOfst+iAmt;
    p->readpoint.pChunk = pChunk;
  }

  return SQLITE_OK;
}

/*
** Free the list of FileChunk structures headed at MemJournal.pFirst.
*/
static void memjrnlFreeChunks(MemJournal *p){
  FileChunk *pIter;
  FileChunk *pNext;
  for(pIter=p->pFirst; pIter; pIter=pNext){
    pNext = pIter->pNext;
    sqlite3_free(pIter);
  } 
  p->pFirst = 0;
}

/*
** Flush the contents of memory to a real file on disk.
*/
static int createFile(MemJournal *p){
  int rc = SQLITE_OK;
  if( !p->pReal ){
    sqlite3_file *pReal = (sqlite3_file *)&p[1];
    rc = sqlite3OsOpen(p->pVfs, p->zJournal, pReal, p->flags, 0);
    if( rc==SQLITE_OK ){
      int nChunk = p->nChunkSize;
      i64 iOff = 0;
      FileChunk *pIter;
      p->pReal = pReal;
      for(pIter=p->pFirst; pIter && rc==SQLITE_OK; pIter=pIter->pNext){
        int nWrite = nChunk;
        if( pIter==p->endpoint.pChunk ){
          nWrite = p->endpoint.iOffset % p->nChunkSize;
          if( nWrite==0 ) nWrite = p->nChunkSize;
        }
        rc = sqlite3OsWrite(pReal, pIter->zChunk, nWrite, iOff);
        iOff += nWrite;
      }
      if( rc!=SQLITE_OK ){
        /* If an error occurred while writing to the file, close it before
        ** returning. This way, SQLite uses the in-memory journal data to 
        ** roll back changes made to the internal page-cache before this
        ** function was called.  */
        sqlite3OsClose(pReal);
        p->pReal = 0;
      }else{
        /* No error has occurred. Free the in-memory buffers. */
        memjrnlFreeChunks(p);
      }
    }
  }
  return rc;
}


/*
** Write data to the file.
*/
static int memjrnlWrite(
  sqlite3_file *pJfd,    /* The journal file into which to write */
  const void *zBuf,      /* Take data to be written from here */
  int iAmt,              /* Number of bytes to write */
  sqlite_int64 iOfst     /* Begin writing at this offset into the file */
){
  MemJournal *p = (MemJournal *)pJfd;
  int nWrite = iAmt;
  u8 *zWrite = (u8 *)zBuf;

  /* If the file has already been created on disk. */
  if( p->pReal ){
    return sqlite3OsWrite(p->pReal, zBuf, iAmt, iOfst);
  }

  /* If the file should be created now. */
  else if( p->nBuf>0 && (iAmt+iOfst)>p->nBuf ){
    int rc = createFile(p);
    if( rc==SQLITE_OK ){
      rc = memjrnlWrite(pJfd, zBuf, iAmt, iOfst);
    }
    return rc;
  }

  /* If the contents of this write should be stored in memory */
  else{
    /* An in-memory journal file should only ever be appended to. Random
    ** access writes are not required. The only exception to this is when
    ** the in-memory journal is being used by a connection using the
    ** atomic-write optimization. In this case the first 28 bytes of the
    ** journal file may be written as part of committing the transaction. */ 
    assert( iOfst==p->endpoint.iOffset || iOfst==0 );
    if( iOfst==0 && p->pFirst ){
      assert( p->nChunkSize>iAmt );
      memcpy(p->pFirst->zChunk, zBuf, iAmt);
    }else{
      while( nWrite>0 ){
        FileChunk *pChunk = p->endpoint.pChunk;
        int iChunkOffset = (int)(p->endpoint.iOffset%p->nChunkSize);
        int iSpace = MIN(nWrite, p->nChunkSize - iChunkOffset);

        if( iChunkOffset==0 ){
          /* New chunk is required to extend the file. */
          FileChunk *pNew = sqlite3_malloc(fileChunkSize(p->nChunkSize));
          if( !pNew ){
            return SQLITE_IOERR_NOMEM_BKPT;
          }
          pNew->pNext = 0;
          if( pChunk ){
            assert( p->pFirst );
            pChunk->pNext = pNew;
          }else{
            assert( !p->pFirst );
            p->pFirst = pNew;
          }
          p->endpoint.pChunk = pNew;
        }

        memcpy(&p->endpoint.pChunk->zChunk[iChunkOffset], zWrite, iSpace);
        zWrite += iSpace;
        nWrite -= iSpace;
        p->endpoint.iOffset += iSpace;
      }
      p->nSize = iAmt + iOfst;
    }
  }

  return SQLITE_OK;
}

/*
** Truncate the file.
**
** If the journal file is already on disk, truncate it there. Or, if it
** is still in main memory but is being truncated to zero bytes in size,
** ignore 
*/
static int memjrnlTruncate(sqlite3_file *pJfd, sqlite_int64 size){
  MemJournal *p = (MemJournal *)pJfd;
  if( p->pReal ){
    return sqlite3OsTruncate(p->pReal, size);
  }else if( size==0 ){
    memjrnlFreeChunks(p);
    p->nSize = 0;
    p->endpoint.pChunk = 0;
    p->endpoint.iOffset = 0;
    p->readpoint.pChunk = 0;
    p->readpoint.iOffset = 0;
  }
  return SQLITE_OK;
}

/*
** Close the file.
*/
static int memjrnlClose(sqlite3_file *pJfd){
  MemJournal *p = (MemJournal *)pJfd;
  memjrnlFreeChunks(p);
  if( p->pReal ) sqlite3OsClose(p->pReal);
  return SQLITE_OK;
}

/*
** Sync the file.
**
** If the real file has been created, call its xSync method. Otherwise, 
** syncing an in-memory journal is a no-op. 
*/
static int memjrnlSync(sqlite3_file *pJfd, int flags){
  MemJournal *p = (MemJournal *)pJfd;
  if( p->pReal ){
    return sqlite3OsSync(p->pReal, flags);
  }
  return SQLITE_OK;
}

/*
** Query the size of the file in bytes.
*/
static int memjrnlFileSize(sqlite3_file *pJfd, sqlite_int64 *pSize){
  MemJournal *p = (MemJournal *)pJfd;
  if( p->pReal ){
    return sqlite3OsFileSize(p->pReal, pSize);
  }
  *pSize = (sqlite_int64) p->endpoint.iOffset;
  return SQLITE_OK;
}

/*
** Table of methods for MemJournal sqlite3_file object.
*/
static const struct sqlite3_io_methods MemJournalMethods = {
  1,                /* iVersion */
  memjrnlClose,     /* xClose */
  memjrnlRead,      /* xRead */
  memjrnlWrite,     /* xWrite */
  memjrnlTruncate,  /* xTruncate */
  memjrnlSync,      /* xSync */
  memjrnlFileSize,  /* xFileSize */
  0,                /* xLock */
  0,                /* xUnlock */
  0,                /* xCheckReservedLock */
  0,                /* xFileControl */
  0,                /* xSectorSize */
  0,                /* xDeviceCharacteristics */
  0,                /* xShmMap */
  0,                /* xShmLock */
  0,                /* xShmBarrier */
  0,                /* xShmUnmap */
  0,                /* xFetch */
  0                 /* xUnfetch */
};

/* 
** Open a journal file. 
**
** The behaviour of the journal file depends on the value of parameter 
** nBuf. If nBuf is 0, then the journal file is always create and 
** accessed using the underlying VFS. If nBuf is less than zero, then
** all content is always stored in main-memory. Finally, if nBuf is a
** positive value, then the journal file is initially created in-memory
** but may be flushed to disk later on. In this case the journal file is
** flushed to disk either when it grows larger than nBuf bytes in size,
** or when sqlite3JournalCreate() is called.
*/
int sqlite3JournalOpen(
  sqlite3_vfs *pVfs,         /* The VFS to use for actual file I/O */
  const char *zName,         /* Name of the journal file */
  sqlite3_file *pJfd,        /* Preallocated, blank file handle */
  int flags,                 /* Opening flags */
  int nBuf                   /* Bytes buffered before opening the file */
){
  MemJournal *p = (MemJournal*)pJfd;

  /* Zero the file-handle object. If nBuf was passed zero, initialize
  ** it using the sqlite3OsOpen() function of the underlying VFS. In this
  ** case none of the code in this module is executed as a result of calls
  ** made on the journal file-handle.  */
  memset(p, 0, sizeof(MemJournal) + (pVfs ? pVfs->szOsFile : 0));
  if( nBuf==0 ){
    return sqlite3OsOpen(pVfs, zName, pJfd, flags, 0);
  }

  if( nBuf>0 ){
    p->nChunkSize = nBuf;
  }else{
    p->nChunkSize = 8 + MEMJOURNAL_DFLT_FILECHUNKSIZE - sizeof(FileChunk);
    assert( MEMJOURNAL_DFLT_FILECHUNKSIZE==fileChunkSize(p->nChunkSize) );
  }

  p->pMethod = (const sqlite3_io_methods*)&MemJournalMethods;
  p->nBuf = nBuf;
  p->flags = flags;
  p->zJournal = zName;
  p->pVfs = pVfs;
  return SQLITE_OK;
}

/*
** Open an in-memory journal file.
*/
void sqlite3MemJournalOpen(sqlite3_file *pJfd){
  sqlite3JournalOpen(0, 0, pJfd, 0, -1);
}

#ifdef SQLITE_ENABLE_ATOMIC_WRITE
/*
** If the argument p points to a MemJournal structure that is not an 
** in-memory-only journal file (i.e. is one that was opened with a +ve
** nBuf parameter), and the underlying file has not yet been created, 
** create it now.
*/
int sqlite3JournalCreate(sqlite3_file *p){
  int rc = SQLITE_OK;
  if( p->pMethods==&MemJournalMethods && ((MemJournal*)p)->nBuf>0 ){
    rc = createFile((MemJournal*)p);
  }
  return rc;
}
#endif

/*
** The file-handle passed as the only argument is open on a journal file.
** Return true if this "journal file" is currently stored in heap memory,
** or false otherwise.
*/
int sqlite3JournalIsInMemory(sqlite3_file *p){
  return p->pMethods==&MemJournalMethods && ((MemJournal*)p)->pReal==0;
}

/* 
** Return the number of bytes required to store a JournalFile that uses vfs
** pVfs to create the underlying on-disk files.
*/
int sqlite3JournalSize(sqlite3_vfs *pVfs){
  return pVfs->szOsFile + sizeof(MemJournal);
}
