/*
** 2005 May 23 
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
** This file contains functions used to access the internal hash tables
** of user defined functions and collation sequences.
*/

#include "sqliteInt.h"

/*
** Connections opened with the SQLITE_OPEN_SHARED_SCHEMA flag specified
** may use SchemaPool objects for any database that is not the temp db
** (iDb==1). For such databases (type "struct Db") there are three states
** the Schema/SchemaPool object may be in.
**
**   1) pSPool==0, pSchema points to an empty object allocated by
**      sqlite3_malloc(). DB_SchemaLoaded flag is clear.
**
**   2) pSPool!=0, pSchema points to a populated object owned by the
**      SchemaPool. DB_SchemaLoaded flag is set.
**
**   3) pSPool!=0, pSchema points to the SchemaPool's static object
**      (SchemaPool.sSchema).
*/
struct SchemaPool {
  int nRef;                       /* Number of pointers to this object */
  int nDelete;                    /* Schema objects deleted by ReleaseAll() */
  u64 cksum;                      /* Checksum for this Schema contents */
  Schema *pSchema;                /* Linked list of Schema objects */
  Schema sSchema;                 /* The single dummy schema object */
  SchemaPool *pNext;              /* Next element in schemaPoolList */
};

#ifdef SQLITE_ENABLE_SHARED_SCHEMA
#ifdef SQLITE_DEBUG
static void assert_schema_state_ok(sqlite3 *db){
  if( IsSharedSchema(db) && db->magic!=SQLITE_MAGIC_ZOMBIE ){
    int i;
    for(i=0; i<db->nDb; i++){
      if( i!=1 ){
        Db *pDb = &db->aDb[i];
        Btree *pBt = pDb->pBt;
        if( pBt==0 ) continue;
        assert( sqlite3BtreeSchema(pBt, 0, 0)==0 );
        assert( pDb->pSchema );
        if( pDb->pSPool ){
          if( DbHasProperty(db, i, DB_SchemaLoaded)==0 ){
            assert( pDb->pSchema->tblHash.count==0 );
            assert( pDb->pSchema==&pDb->pSPool->sSchema );
          }else{
            assert( pDb->pSchema!=&pDb->pSPool->sSchema );
          }
        }else{
          assert( DbHasProperty(db, i, DB_SchemaLoaded)==0 );
          assert( pDb->pSchema->tblHash.count==0 );
          assert( pDb->pSchema!=&pDb->pSPool->sSchema );
        }
      }
    }
  }
}
#else
# define assert_schema_state_ok(x)
#endif
#endif /* ifdef SQLITE_ENABLE_SHARED_SCHEMA */

/*
** Invoke the 'collation needed' callback to request a collation sequence
** in the encoding enc of name zName, length nName.
*/
static void callCollNeeded(sqlite3 *db, int enc, const char *zName){
  assert( !db->xCollNeeded || !db->xCollNeeded16 );
  if( db->xCollNeeded ){
    char *zExternal = sqlite3DbStrDup(db, zName);
    if( !zExternal ) return;
    db->xCollNeeded(db->pCollNeededArg, db, enc, zExternal);
    sqlite3DbFree(db, zExternal);
  }
#ifndef SQLITE_OMIT_UTF16
  if( db->xCollNeeded16 ){
    char const *zExternal;
    sqlite3_value *pTmp = sqlite3ValueNew(db);
    sqlite3ValueSetStr(pTmp, -1, zName, SQLITE_UTF8, SQLITE_STATIC);
    zExternal = sqlite3ValueText(pTmp, SQLITE_UTF16NATIVE);
    if( zExternal ){
      db->xCollNeeded16(db->pCollNeededArg, db, (int)ENC(db), zExternal);
    }
    sqlite3ValueFree(pTmp);
  }
#endif
}

/*
** This routine is called if the collation factory fails to deliver a
** collation function in the best encoding but there may be other versions
** of this collation function (for other text encodings) available. Use one
** of these instead if they exist. Avoid a UTF-8 <-> UTF-16 conversion if
** possible.
*/
static int synthCollSeq(sqlite3 *db, CollSeq *pColl){
  CollSeq *pColl2;
  char *z = pColl->zName;
  int i;
  static const u8 aEnc[] = { SQLITE_UTF16BE, SQLITE_UTF16LE, SQLITE_UTF8 };
  for(i=0; i<3; i++){
    pColl2 = sqlite3FindCollSeq(db, aEnc[i], z, 0);
    if( pColl2->xCmp!=0 ){
      memcpy(pColl, pColl2, sizeof(CollSeq));
      pColl->xDel = 0;         /* Do not copy the destructor */
      return SQLITE_OK;
    }
  }
  return SQLITE_ERROR;
}

/*
** This routine is called on a collation sequence before it is used to
** check that it is defined. An undefined collation sequence exists when
** a database is loaded that contains references to collation sequences
** that have not been defined by sqlite3_create_collation() etc.
**
** If required, this routine calls the 'collation needed' callback to
** request a definition of the collating sequence. If this doesn't work, 
** an equivalent collating sequence that uses a text encoding different
** from the main database is substituted, if one is available.
*/
int sqlite3CheckCollSeq(Parse *pParse, CollSeq *pColl){
  if( pColl && pColl->xCmp==0 ){
    const char *zName = pColl->zName;
    sqlite3 *db = pParse->db;
    CollSeq *p = sqlite3GetCollSeq(pParse, ENC(db), pColl, zName);
    if( !p ){
      return SQLITE_ERROR;
    }
    assert( p==pColl );
  }
  return SQLITE_OK;
}



/*
** Locate and return an entry from the db.aCollSeq hash table. If the entry
** specified by zName and nName is not found and parameter 'create' is
** true, then create a new entry. Otherwise return NULL.
**
** Each pointer stored in the sqlite3.aCollSeq hash table contains an
** array of three CollSeq structures. The first is the collation sequence
** preferred for UTF-8, the second UTF-16le, and the third UTF-16be.
**
** Stored immediately after the three collation sequences is a copy of
** the collation sequence name. A pointer to this string is stored in
** each collation sequence structure.
*/
static CollSeq *findCollSeqEntry(
  sqlite3 *db,          /* Database connection */
  const char *zName,    /* Name of the collating sequence */
  int create            /* Create a new entry if true */
){
  CollSeq *pColl;
  pColl = sqlite3HashFind(&db->aCollSeq, zName);

  if( 0==pColl && create ){
    int nName = sqlite3Strlen30(zName) + 1;
    pColl = sqlite3DbMallocZero(db, 3*sizeof(*pColl) + nName);
    if( pColl ){
      CollSeq *pDel = 0;
      pColl[0].zName = (char*)&pColl[3];
      pColl[0].enc = SQLITE_UTF8;
      pColl[1].zName = (char*)&pColl[3];
      pColl[1].enc = SQLITE_UTF16LE;
      pColl[2].zName = (char*)&pColl[3];
      pColl[2].enc = SQLITE_UTF16BE;
      memcpy(pColl[0].zName, zName, nName);
      pDel = sqlite3HashInsert(&db->aCollSeq, pColl[0].zName, pColl);

      /* If a malloc() failure occurred in sqlite3HashInsert(), it will 
      ** return the pColl pointer to be deleted (because it wasn't added
      ** to the hash table).
      */
      assert( pDel==0 || pDel==pColl );
      if( pDel!=0 ){
        sqlite3OomFault(db);
        sqlite3DbFree(db, pDel);
        pColl = 0;
      }
    }
  }
  return pColl;
}

/*
** Parameter zName points to a UTF-8 encoded string nName bytes long.
** Return the CollSeq* pointer for the collation sequence named zName
** for the encoding 'enc' from the database 'db'.
**
** If the entry specified is not found and 'create' is true, then create a
** new entry.  Otherwise return NULL.
**
** A separate function sqlite3LocateCollSeq() is a wrapper around
** this routine.  sqlite3LocateCollSeq() invokes the collation factory
** if necessary and generates an error message if the collating sequence
** cannot be found.
**
** See also: sqlite3LocateCollSeq(), sqlite3GetCollSeq()
*/
CollSeq *sqlite3FindCollSeq(
  sqlite3 *db,          /* Database connection to search */
  u8 enc,               /* Desired text encoding */
  const char *zName,    /* Name of the collating sequence.  Might be NULL */
  int create            /* True to create CollSeq if doesn't already exist */
){
  CollSeq *pColl;
  if( zName ){
    pColl = findCollSeqEntry(db, zName, create);
  }else{
    pColl = db->pDfltColl;
  }
  assert( SQLITE_UTF8==1 && SQLITE_UTF16LE==2 && SQLITE_UTF16BE==3 );
  assert( enc>=SQLITE_UTF8 && enc<=SQLITE_UTF16BE );
  if( pColl ) pColl += enc-1;
  return pColl;
}

/*
** This function is responsible for invoking the collation factory callback
** or substituting a collation sequence of a different encoding when the
** requested collation sequence is not available in the desired encoding.
** 
** If it is not NULL, then pColl must point to the database native encoding 
** collation sequence with name zName, length nName.
**
** The return value is either the collation sequence to be used in database
** db for collation type name zName, length nName, or NULL, if no collation
** sequence can be found.  If no collation is found, leave an error message.
**
** See also: sqlite3LocateCollSeq(), sqlite3FindCollSeq()
*/
CollSeq *sqlite3GetCollSeq(
  Parse *pParse,        /* Parsing context */
  u8 enc,               /* The desired encoding for the collating sequence */
  CollSeq *pColl,       /* Collating sequence with native encoding, or NULL */
  const char *zName     /* Collating sequence name */
){
  CollSeq *p;
  sqlite3 *db = pParse->db;

  p = pColl;
  if( !p ){
    p = sqlite3FindCollSeq(db, enc, zName, 0);
  }
  if( !p || !p->xCmp ){
    /* No collation sequence of this type for this encoding is registered.
    ** Call the collation factory to see if it can supply us with one.
    */
    callCollNeeded(db, enc, zName);
    p = sqlite3FindCollSeq(db, enc, zName, 0);
  }
  if( p && !p->xCmp && synthCollSeq(db, p) ){
    p = 0;
  }
  assert( !p || p->xCmp );
  if( p==0 ){
    sqlite3ErrorMsg(pParse, "no such collation sequence: %s", zName);
    pParse->rc = SQLITE_ERROR_MISSING_COLLSEQ;
  }
  return p;
}

/*
** This function returns the collation sequence for database native text
** encoding identified by the string zName.
**
** If the requested collation sequence is not available, or not available
** in the database native encoding, the collation factory is invoked to
** request it. If the collation factory does not supply such a sequence,
** and the sequence is available in another text encoding, then that is
** returned instead.
**
** If no versions of the requested collations sequence are available, or
** another error occurs, NULL is returned and an error message written into
** pParse.
**
** This routine is a wrapper around sqlite3FindCollSeq().  This routine
** invokes the collation factory if the named collation cannot be found
** and generates an error message.
**
** See also: sqlite3FindCollSeq(), sqlite3GetCollSeq()
*/
CollSeq *sqlite3LocateCollSeq(Parse *pParse, const char *zName){
  sqlite3 *db = pParse->db;
  u8 enc = ENC(db);
  u8 initbusy = db->init.busy;
  CollSeq *pColl;

  pColl = sqlite3FindCollSeq(db, enc, zName, initbusy);
  if( !initbusy && (!pColl || !pColl->xCmp) ){
    pColl = sqlite3GetCollSeq(pParse, enc, pColl, zName);
  }

  return pColl;
}

/* During the search for the best function definition, this procedure
** is called to test how well the function passed as the first argument
** matches the request for a function with nArg arguments in a system
** that uses encoding enc. The value returned indicates how well the
** request is matched. A higher value indicates a better match.
**
** If nArg is -1 that means to only return a match (non-zero) if p->nArg
** is also -1.  In other words, we are searching for a function that
** takes a variable number of arguments.
**
** If nArg is -2 that means that we are searching for any function 
** regardless of the number of arguments it uses, so return a positive
** match score for any
**
** The returned value is always between 0 and 6, as follows:
**
** 0: Not a match.
** 1: UTF8/16 conversion required and function takes any number of arguments.
** 2: UTF16 byte order change required and function takes any number of args.
** 3: encoding matches and function takes any number of arguments
** 4: UTF8/16 conversion required - argument count matches exactly
** 5: UTF16 byte order conversion required - argument count matches exactly
** 6: Perfect match:  encoding and argument count match exactly.
**
** If nArg==(-2) then any function with a non-null xSFunc is
** a perfect match and any function with xSFunc NULL is
** a non-match.
*/
#define FUNC_PERFECT_MATCH 6  /* The score for a perfect match */
static int matchQuality(
  FuncDef *p,     /* The function we are evaluating for match quality */
  int nArg,       /* Desired number of arguments.  (-1)==any */
  u8 enc          /* Desired text encoding */
){
  int match;
  assert( p->nArg>=-1 );

  /* Wrong number of arguments means "no match" */
  if( p->nArg!=nArg ){
    if( nArg==(-2) ) return (p->xSFunc==0) ? 0 : FUNC_PERFECT_MATCH;
    if( p->nArg>=0 ) return 0;
  }

  /* Give a better score to a function with a specific number of arguments
  ** than to function that accepts any number of arguments. */
  if( p->nArg==nArg ){
    match = 4;
  }else{
    match = 1;
  }

  /* Bonus points if the text encoding matches */
  if( enc==(p->funcFlags & SQLITE_FUNC_ENCMASK) ){
    match += 2;  /* Exact encoding match */
  }else if( (enc & p->funcFlags & 2)!=0 ){
    match += 1;  /* Both are UTF16, but with different byte orders */
  }

  return match;
}

/*
** Search a FuncDefHash for a function with the given name.  Return
** a pointer to the matching FuncDef if found, or 0 if there is no match.
*/
FuncDef *sqlite3FunctionSearch(
  int h,               /* Hash of the name */
  const char *zFunc    /* Name of function */
){
  FuncDef *p;
  for(p=sqlite3BuiltinFunctions.a[h]; p; p=p->u.pHash){
    if( sqlite3StrICmp(p->zName, zFunc)==0 ){
      return p;
    }
  }
  return 0;
}

/*
** Insert a new FuncDef into a FuncDefHash hash table.
*/
void sqlite3InsertBuiltinFuncs(
  FuncDef *aDef,      /* List of global functions to be inserted */
  int nDef            /* Length of the apDef[] list */
){
  int i;
  for(i=0; i<nDef; i++){
    FuncDef *pOther;
    const char *zName = aDef[i].zName;
    int nName = sqlite3Strlen30(zName);
    int h = SQLITE_FUNC_HASH(zName[0], nName);
    assert( zName[0]>='a' && zName[0]<='z' );
    pOther = sqlite3FunctionSearch(h, zName);
    if( pOther ){
      assert( pOther!=&aDef[i] && pOther->pNext!=&aDef[i] );
      aDef[i].pNext = pOther->pNext;
      pOther->pNext = &aDef[i];
    }else{
      aDef[i].pNext = 0;
      aDef[i].u.pHash = sqlite3BuiltinFunctions.a[h];
      sqlite3BuiltinFunctions.a[h] = &aDef[i];
    }
  }
}
  
  

/*
** Locate a user function given a name, a number of arguments and a flag
** indicating whether the function prefers UTF-16 over UTF-8.  Return a
** pointer to the FuncDef structure that defines that function, or return
** NULL if the function does not exist.
**
** If the createFlag argument is true, then a new (blank) FuncDef
** structure is created and liked into the "db" structure if a
** no matching function previously existed.
**
** If nArg is -2, then the first valid function found is returned.  A
** function is valid if xSFunc is non-zero.  The nArg==(-2)
** case is used to see if zName is a valid function name for some number
** of arguments.  If nArg is -2, then createFlag must be 0.
**
** If createFlag is false, then a function with the required name and
** number of arguments may be returned even if the eTextRep flag does not
** match that requested.
*/
FuncDef *sqlite3FindFunction(
  sqlite3 *db,       /* An open database */
  const char *zName, /* Name of the function.  zero-terminated */
  int nArg,          /* Number of arguments.  -1 means any number */
  u8 enc,            /* Preferred text encoding */
  u8 createFlag      /* Create new entry if true and does not otherwise exist */
){
  FuncDef *p;         /* Iterator variable */
  FuncDef *pBest = 0; /* Best match found so far */
  int bestScore = 0;  /* Score of best match */
  int h;              /* Hash value */
  int nName;          /* Length of the name */

  assert( nArg>=(-2) );
  assert( nArg>=(-1) || createFlag==0 );
  nName = sqlite3Strlen30(zName);

  /* First search for a match amongst the application-defined functions.
  */
  p = (FuncDef*)sqlite3HashFind(&db->aFunc, zName);
  while( p ){
    int score = matchQuality(p, nArg, enc);
    if( score>bestScore ){
      pBest = p;
      bestScore = score;
    }
    p = p->pNext;
  }

  /* If no match is found, search the built-in functions.
  **
  ** If the DBFLAG_PreferBuiltin flag is set, then search the built-in
  ** functions even if a prior app-defined function was found.  And give
  ** priority to built-in functions.
  **
  ** Except, if createFlag is true, that means that we are trying to
  ** install a new function.  Whatever FuncDef structure is returned it will
  ** have fields overwritten with new information appropriate for the
  ** new function.  But the FuncDefs for built-in functions are read-only.
  ** So we must not search for built-ins when creating a new function.
  */ 
  if( !createFlag && (pBest==0 || (db->mDbFlags & DBFLAG_PreferBuiltin)!=0) ){
    bestScore = 0;
    h = SQLITE_FUNC_HASH(sqlite3UpperToLower[(u8)zName[0]], nName);
    p = sqlite3FunctionSearch(h, zName);
    while( p ){
      int score = matchQuality(p, nArg, enc);
      if( score>bestScore ){
        pBest = p;
        bestScore = score;
      }
      p = p->pNext;
    }
  }

  /* If the createFlag parameter is true and the search did not reveal an
  ** exact match for the name, number of arguments and encoding, then add a
  ** new entry to the hash table and return it.
  */
  if( createFlag && bestScore<FUNC_PERFECT_MATCH && 
      (pBest = sqlite3DbMallocZero(db, sizeof(*pBest)+nName+1))!=0 ){
    FuncDef *pOther;
    u8 *z;
    pBest->zName = (const char*)&pBest[1];
    pBest->nArg = (u16)nArg;
    pBest->funcFlags = enc;
    memcpy((char*)&pBest[1], zName, nName+1);
    for(z=(u8*)pBest->zName; *z; z++) *z = sqlite3UpperToLower[*z];
    pOther = (FuncDef*)sqlite3HashInsert(&db->aFunc, pBest->zName, pBest);
    if( pOther==pBest ){
      sqlite3DbFree(db, pBest);
      sqlite3OomFault(db);
      return 0;
    }else{
      pBest->pNext = pOther;
    }
  }

  if( pBest && (pBest->xSFunc || createFlag) ){
    return pBest;
  }
  return 0;
}

/*
** Free all resources held by the schema structure. The void* argument points
** at a Schema struct. This function does not call sqlite3DbFree(db, ) on the 
** pointer itself, it just cleans up subsidiary resources (i.e. the contents
** of the schema hash tables).
**
** The Schema.cache_size variable is not cleared.
*/
void sqlite3SchemaClear(void *p){
  Hash temp1;
  Hash temp2;
  HashElem *pElem;
  Schema *pSchema = (Schema *)p;

  temp1 = pSchema->tblHash;
  temp2 = pSchema->trigHash;
  sqlite3HashInit(&pSchema->trigHash);
  sqlite3HashClear(&pSchema->idxHash);
  for(pElem=sqliteHashFirst(&temp2); pElem; pElem=sqliteHashNext(pElem)){
    sqlite3DeleteTrigger(0, (Trigger*)sqliteHashData(pElem));
  }
  sqlite3HashClear(&temp2);
  sqlite3HashInit(&pSchema->tblHash);
  for(pElem=sqliteHashFirst(&temp1); pElem; pElem=sqliteHashNext(pElem)){
    Table *pTab = sqliteHashData(pElem);
    sqlite3DeleteTable(0, pTab);
  }
  sqlite3HashClear(&temp1);
  sqlite3HashClear(&pSchema->fkeyHash);
  pSchema->pSeqTab = 0;
  if( pSchema->schemaFlags & DB_SchemaLoaded ){
    pSchema->iGeneration++;
  }
  pSchema->schemaFlags &= ~(DB_SchemaLoaded|DB_ResetWanted);
}

/*
** If this database was opened with the SQLITE_OPEN_SHARED_SCHEMA flag
** and iDb!=1, then disconnect from the schema-pool associated with
** database iDb. Otherwise, clear the Schema object belonging to
** database iDb. 
**
** If an OOM error occurs while disconnecting from a schema-pool, 
** the db->mallocFailed flag is set.
*/
void sqlite3SchemaClearOrDisconnect(sqlite3 *db, int iDb){
  Db *pDb = &db->aDb[iDb];
#ifdef SQLITE_ENABLE_SHARED_SCHEMA
  if( IsSharedSchema(db) && iDb!=1 && pDb->pSPool ){
    sqlite3SchemaDisconnect(db, iDb, 1);
  }else
#endif
  {
    sqlite3SchemaClear(pDb->pSchema);
  }
}

#ifdef SQLITE_ENABLE_SHARED_SCHEMA
/*
** Global linked list of SchemaPool objects. Read and write access must
** be protected by the SQLITE_MUTEX_STATIC_MASTER mutex.
*/
static SchemaPool *SQLITE_WSD schemaPoolList = 0;

#ifdef SQLITE_TEST
/*
** Return a pointer to the head of the linked list of SchemaPool objects.
** This is used by the virtual table in file test_schemapool.c.
*/
SchemaPool *sqlite3SchemaPoolList(void){ return schemaPoolList; }
#endif

/*
** Database handle db was opened with the SHARED_SCHEMA flag, and database
** iDb is currently connected to a schema-pool. When this function is called,
** (*pnByte) is set to nInit plus the amount of memory used to store a 
** single instance of the Schema objects managed by the schema-pool.
** This function adjusts (*pnByte) sot hat it is set to nInit plus
** (nSchema/nRef) of the amount of memory used by a single Schema object,
** where nSchema is the number of Schema objects allocated by this pool,
** and nRef is the number of connections to the schema-pool.
*/
void sqlite3SchemaAdjustUsed(sqlite3 *db, int iDb, int nInit, int *pnByte){
  SchemaPool *pSPool = db->aDb[iDb].pSPool;
  int nSchema = 0;
  Schema *p;
  sqlite3_mutex_enter( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
  for(p=pSPool->pSchema; p; p=p->pNext){
    nSchema++;
  }
  *pnByte = nInit + ((*pnByte - nInit) * nSchema) / pSPool->nRef;
  sqlite3_mutex_leave( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
}

/*
** Check that the schema of db iDb is writable (either because it is the 
** temp db schema or because the db handle was opened without
** SQLITE_OPEN_SHARED_SCHEMA). If so, do nothing. Otherwise, leave an 
** error in the Parse object.
*/
void sqlite3SchemaWritable(Parse *pParse, int iDb){
  if( iDb!=1 && IsSharedSchema(pParse->db) && IN_DECLARE_VTAB==0 ){
    sqlite3ErrorMsg(pParse, "attempt to modify read-only schema");
  }
}

/*
** The schema object passed as the only argument was allocated using
** sqlite3_malloc() and then populated using the usual mechanism. This
** function frees both the Schema object and its contents.
*/
static void schemaDelete(Schema *pSchema){
  sqlite3SchemaClear((void*)pSchema);
  sqlite3_free(pSchema);
}

/*
** When this function is called, the database connection Db must be
** using a schema-pool (Db.pSPool!=0) and must currently have Db.pSchema
** set to point to a populated schema object checked out from the 
** schema-pool. It is also assumed that the STATIC_MASTER mutex is held.
** This function returns the Schema object to the schema-pool and sets
** Db.pSchema to point to the schema-pool's static, empty, Schema object.
*/
static void schemaRelease(sqlite3 *db, Db *pDb){
  Schema *pRelease = pDb->pSchema;
  SchemaPool *pSPool = pDb->pSPool;

  assert( pDb->pSchema->iGeneration==pSPool->sSchema.iGeneration );
  pDb->pSchema = &pSPool->sSchema;

  assert( pDb->pSPool && pRelease );
  assert( pRelease->schemaFlags & DB_SchemaLoaded );
  assert( (pDb->pSchema->schemaFlags & DB_SchemaLoaded)==0 );
  assert( sqlite3_mutex_held(sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER)) );

  /* If the DBFLAG_FreeSchema flag is set and the database connection holds
  ** at least one other copy of the schema being released, delete it instead
  ** of returning it to the schema-pool.  */
  if( db->mDbFlags & DBFLAG_FreeSchema ){
    int i;
    for(i=0; i<db->nDb; i++){
      Db *p = &db->aDb[i];
      if( p!=pDb && p->pSchema!=&pSPool->sSchema && pDb->pSPool==p->pSPool ){
        pSPool->nDelete++;
        schemaDelete(pRelease);
        return;
      }
    }
  }

  pRelease->pNext = pDb->pSPool->pSchema;
  pDb->pSPool->pSchema = pRelease;
}

/*
** The schema for database iDb of database handle db, which was opened
** with SQLITE_OPEN_SHARED_SCHEMA, has just been parsed. This function either
** finds a matching SchemaPool object on the global list (schemaPoolList) or
** else allocates a new one and sets the Db.pSPool variable accordingly.
**
** SQLITE_OK is returned if no error occurs, or an SQLite error code 
** (SQLITE_NOMEM) otherwise.
*/
int sqlite3SchemaConnect(sqlite3 *db, int iDb, u64 cksum){
  Schema *pSchema = db->aDb[iDb].pSchema;
  SchemaPool *p;

  assert( pSchema && iDb!=1 && db->aDb[iDb].pSPool==0 );

  sqlite3_mutex_enter( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );

  /* Search for a matching SchemaPool object */
  for(p=schemaPoolList; p; p=p->pNext){
    if( p->cksum==cksum && p->sSchema.schema_cookie==pSchema->schema_cookie ){
      break;
    }
  }
  if( !p ){
    /* No SchemaPool object found. Allocate a new one. */
    p = (SchemaPool*)sqlite3_malloc(sizeof(SchemaPool));
    if( p ){
      memset(p, 0, sizeof(SchemaPool));
      p->cksum = cksum;
      p->pNext = schemaPoolList;
      schemaPoolList = p;

      p->sSchema.schema_cookie = pSchema->schema_cookie;
      p->sSchema.iGeneration = pSchema->iGeneration;
      p->sSchema.file_format = pSchema->file_format;
      p->sSchema.enc = pSchema->enc;
      p->sSchema.cache_size = pSchema->cache_size;
    }
  }

  if( p ) p->nRef++;

  /* If the SchemaPool contains one or more free schemas at the moment, 
  ** delete one of them. */
  if( p && p->pSchema ){
    Schema *pDel = p->pSchema;
    p->pSchema = pDel->pNext;
    schemaDelete(pDel);
  }

  sqlite3_mutex_leave( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );

  db->aDb[iDb].pSPool = p;
  return (p ? SQLITE_OK : SQLITE_NOMEM);
}

/*
** If parameter iDb is 1 (the temp db), or if connection handle db was not
** opened with the SQLITE_OPEN_SHARED_SCHEMA flag, this function is a no-op.
** Otherwise, it disconnects from the schema-pool associated with database
** iDb, assuming it is connected.
**
** If parameter bNew is true, then Db.pSchema is set to point to a new, empty,
** Schema object obtained from sqlite3_malloc(). Or, if bNew is false, then
** Db.pSchema is set to NULL before returning.
**
** If the bNew parameter is true, then this function may allocate memory. 
** If the allocation attempt fails, then SQLITE_NOMEM is returned and the
** schema-pool is not disconnected from. Or, if no OOM error occurs, 
** SQLITE_OK is returned.
*/
int sqlite3SchemaDisconnect(sqlite3 *db, int iDb, int bNew){
  int rc = SQLITE_OK;
  if( IsSharedSchema(db) ){
    Db *pDb = &db->aDb[iDb];
    SchemaPool *pSPool = pDb->pSPool;
    assert_schema_state_ok(db);
    assert( pDb->pSchema );

    if( pSPool==0 ){
      assert( pDb->pVTable==0 );
      assert( bNew==0 );
      schemaDelete(pDb->pSchema);
      pDb->pSchema = 0;
    }else{
      VTable *p;
      VTable *pNext;
      for(p=pDb->pVTable; p; p=pNext){
        pNext = p->pNext;
        sqlite3VtabUnlock(p);
      }
      pDb->pVTable = 0;
      sqlite3_mutex_enter( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
      if( DbHasProperty(db, iDb, DB_SchemaLoaded) ){
        schemaRelease(db, pDb);
      }
      if( bNew ){
        Schema *pNew = sqlite3SchemaGet(db, 0);
        if( pNew==0 ){
          rc = SQLITE_NOMEM;
        }else{
          pDb->pSchema = pNew;
        }
      }
      if( rc==SQLITE_OK ){
        assert( pSPool->nRef>=1 );
        pDb->pSPool = 0;
        pSPool->nRef--;
        if( pSPool->nRef<=0 ){
          SchemaPool **pp;
          while( pSPool->pSchema ){
            Schema *pNext = pSPool->pSchema->pNext;
            schemaDelete(pSPool->pSchema);
            pSPool->pSchema = pNext;
          }
          for(pp=&schemaPoolList; (*pp)!=pSPool; pp=&((*pp)->pNext));
          *pp = pSPool->pNext;
          sqlite3_free(pSPool);
        }
      }
      sqlite3_mutex_leave( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
    }
  }
  return rc;
}

/*
** Extract and return a pointer to a schema object from the SchemaPool passed
** as the only argument, if one is available. If one is not available, return
** NULL.
*/
Schema *sqlite3SchemaExtract(SchemaPool *pSPool){
  Schema *pRet = 0;
  sqlite3_mutex_enter( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
  if( pSPool->pSchema ){
    pRet = pSPool->pSchema;
    pSPool->pSchema = pRet->pNext;
    pRet->pNext = 0;
  }
  sqlite3_mutex_leave( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
  return pRet;
}

/*
** Return all sharable schemas held by database handle db back to their
** respective schema-pools. Db.pSchema variables are left pointing to
** the static, empty, Schema object owned by each schema-pool.
*/
void sqlite3SchemaReleaseAll(sqlite3 *db){
  int i;
  assert_schema_state_ok(db);
  sqlite3_mutex_enter( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
  for(i=0; i<db->nDb; i++){
    if( i!=1 ){
      Db *pDb = &db->aDb[i];
      if( pDb->pSPool && DbHasProperty(db,i,DB_SchemaLoaded) ){
        schemaRelease(db, pDb);
      }
    }
  }
  db->mDbFlags &= ~DBFLAG_FreeSchema;
  sqlite3_mutex_leave( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
}

/*
** Release any sharable schema held by connection iDb of database handle
** db. Db.pSchema is left pointing to the static, empty, Schema object
** owned by the schema-pool.
*/
void sqlite3SchemaRelease(sqlite3 *db, int iDb){
  Db *pDb = &db->aDb[iDb];
  assert( iDb!=1 );
  assert_schema_state_ok(db);
  sqlite3_mutex_enter( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
  schemaRelease(db, pDb);
  sqlite3_mutex_leave( sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER) );
}

#endif /* ifdef SQLITE_ENABLE_SHARED_SCHEMA */

/*
** In most cases, this function finds and returns the schema associated 
** with BTree handle pBt, creating a new one if necessary. However, if
** the database handle was opened with the SQLITE_OPEN_SHARED_SCHEMA flag
** specified, a new, empty, Schema object in memory obtained by 
** sqlite3_malloc() is always returned.
*/
Schema *sqlite3SchemaGet(sqlite3 *db, Btree *pBt){
  Schema *p;
  if( pBt && IsSharedSchema(db)==0 ){
    p = (Schema*)sqlite3BtreeSchema(pBt, sizeof(Schema), sqlite3SchemaClear);
  }else{
    p = (Schema*)sqlite3DbMallocZero(0, sizeof(Schema));
  }
  if( !p ){
    sqlite3OomFault(db);
  }else if ( 0==p->file_format ){
    sqlite3HashInit(&p->tblHash);
    sqlite3HashInit(&p->idxHash);
    sqlite3HashInit(&p->trigHash);
    sqlite3HashInit(&p->fkeyHash);
    p->enc = SQLITE_UTF8;
  }
  return p;
}
