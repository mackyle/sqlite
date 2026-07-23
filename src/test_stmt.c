/*
** 2026-07-21
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
** The code in this file tests interfaces that are not directly 
** accessible to Tcl scripts. 
*/

#include "sqliteInt.h"
#include "tclsqlite.h"

typedef struct TestStatic TestStatic;
struct TestStatic {
  void *pBuf;
  TestStatic *pNext;
};

typedef struct TestStmt TestStmt;
struct TestStmt {
  sqlite3_stmt *pStmt;
  TestStatic *pStatic;
};

extern const char *sqlite3ErrName(int);
extern int getDbPointer(Tcl_Interp *, const char *, sqlite3 **);

typedef struct TestPrepareContext TestPrepareContext;
struct TestPrepareContext {
  int iCnt;
};

#define OPT_TYPE_BOOLEAN    1
#define OPT_TYPE_INTSWITCH  2
#define OPT_TYPE_STRSWITCH  3
#define OPT_TYPE_POSITION   4

typedef struct CmdOption CmdOption;
struct CmdOption {
  const char *zName;
  int eType;
  void *pVal;
};

static int testProcessOptions(
  Tcl_Interp *interp,
  int nObj,
  Tcl_Obj *CONST aObj[],
  CmdOption *aOpt,
  int nUsed,
  int nUseInUsage,
  const char *zUsage
){
  int ii;

  for(ii=nUsed; ii<nObj; ii++){
    char *zArg = Tcl_GetString(aObj[ii]);
    if( zArg[0]!='-' ){
      /* A positional argument */
      int jj;
      for(jj=0; aOpt[jj].zName; jj++){
        if( aOpt[jj].eType==OPT_TYPE_POSITION ){
          *(const char**)aOpt[jj].pVal = zArg;
          aOpt[jj].eType = 0;
          break;
        }
      }
      if( aOpt[jj].zName==0 ) goto usage;
    }else{
      CmdOption *pOpt = 0;
      int iOpt = 0;
      if( Tcl_GetIndexFromObjStruct(
            interp, aObj[ii], aOpt, sizeof(CmdOption), "option", 0, &iOpt
      )){
        return TCL_ERROR;
      }
      pOpt = &aOpt[iOpt];

      if( pOpt->eType==OPT_TYPE_BOOLEAN ){
        *(int*)pOpt->pVal = 1;
      }else if( ii==(nObj-1) ){
        Tcl_AppendResult(
            interp, "option requires an argument: ", Tcl_GetString(aObj[ii]),
            (char*)0
        );
        return TCL_ERROR;
      }else if( pOpt->eType==OPT_TYPE_INTSWITCH ){
        ii++;
        if( Tcl_GetIntFromObj(interp, aObj[ii], (int*)pOpt->pVal) ){
          return TCL_ERROR;
        }
      }else if( pOpt->eType==OPT_TYPE_STRSWITCH ){
        ii++;
        *(char**)pOpt->pVal = Tcl_GetString(aObj[ii]);
      }
    }
  }
  return TCL_OK;

 usage:
  Tcl_WrongNumArgs(interp, nUseInUsage, aObj, zUsage);
  return TCL_ERROR;
}

/*
** Set the result of the interpreter passed as the first argument to the
** string representation of the SQLite error code passed as the second.
*/
static void testSetInterpResult(Tcl_Interp *interp, int rc){
  Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
}

static void del_stmt_cmd(ClientData clientData){
  TestStmt *p = (TestStmt*)clientData;
  TestStatic *pStatic;
  TestStatic *pNext;
  sqlite3_finalize(p->pStmt);
  for(pStatic=p->pStatic; pStatic; pStatic=pNext){
    pNext = pStatic->pNext;
    ckfree(pStatic);
  }
  ckfree(p);
}

static int testBindDouble(
  Tcl_Interp *interp,
  sqlite3_stmt *pStmt, 
  int iBind,
  Tcl_Obj *pObj
){
  static const struct {
    const char *zName;     /* Name of the special floating point value */
    unsigned int iUpper;   /* Upper 32 bits */
    unsigned int iLower;   /* Lower 32 bits */
  } aSpecialFp[] = {
    {  "NaN",      0x7fffffff, 0xffffffff },
    {  "SNaN",     0x7ff7ffff, 0xffffffff },
    {  "-NaN",     0xffffffff, 0xffffffff },
    {  "-SNaN",    0xfff7ffff, 0xffffffff },
    {  "+Inf",     0x7ff00000, 0x00000000 },
    {  "-Inf",     0xfff00000, 0x00000000 },
    {  "Epsilon",  0x00000000, 0x00000001 },
    {  "-Epsilon", 0x80000000, 0x00000001 },
    {  "NaN0",     0x7ff80000, 0x00000000 },
    {  "-NaN0",    0xfff80000, 0x00000000 },
  };
  double value = 0;
  const char *zVal = Tcl_GetString(pObj);
  int ii;
  int rc;

  for(ii=0; ii<sizeof(aSpecialFp)/sizeof(aSpecialFp[0]); ii++){
    if( strcmp(aSpecialFp[ii].zName, zVal)==0 ){
      sqlite3_uint64 x;
      x = aSpecialFp[ii].iUpper;
      x <<= 32;
      x |= aSpecialFp[ii].iLower;
      assert( sizeof(value)==8 );
      assert( sizeof(x)==8 );
      memcpy(&value, &x, 8);
      break;
    }
  }

  if( ii>=sizeof(aSpecialFp)/sizeof(aSpecialFp[0])
   && Tcl_GetDoubleFromObj(interp, pObj, &value)
  ){
    return TCL_ERROR;
  }

  rc = sqlite3_bind_double(pStmt, iBind, value);
  testSetInterpResult(interp, rc);

  return TCL_OK;
}


static const char *testMakeStatic(TestStmt *p, const char *zText){
  int nText = strlen(zText);
  int nByte = nText + sizeof(TestStatic) + 1;
  TestStatic *pNew = 0;

  pNew = (TestStatic*)ckalloc(nByte);
  memset(pNew, 0, nByte);
  pNew->pBuf = (void*)&pNew[1];
  pNew->pNext = p->pStatic;
  p->pStatic = pNew;
  memcpy(pNew->pBuf, zText, nText);
  return pNew->pBuf;
}

static const unsigned char *testMakeStaticBlob(
  TestStmt *p, 
  const unsigned char *pVal, 
  int nVal
){
  int nByte = nVal + sizeof(TestStatic);
  TestStatic *pNew = 0;

  pNew = (TestStatic*)ckalloc(nByte);
  memset(pNew, 0, nByte);
  pNew->pBuf = (void*)&pNew[1];
  pNew->pNext = p->pStatic;
  p->pStatic = pNew;
  memcpy(pNew->pBuf, pVal, nVal);
  return (const unsigned char*)pNew->pBuf;
}

/*
** Tclcmd:
**
**   $stmt SUB-COMMAND
*/
static int SQLITE_TCLAPI test_stmt_cmd(
  ClientData clientData, /* Pointer to sqlite3_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  struct SubCmd {
    const char *zSub;
    int nArg;
    const char *zUsage;
  } aSub[] = {
    {"step", 0, ""},                          /* 0 */
    {"finalize", 0, ""},                      /* 1 */
    {"bind_double", 2, "IVAR VALUE"},         /* 2 */
    {"reset", 0, ""},                         /* 3 */
    {"bind_zeroblob", 2, "IVAR NBYTE"},       /* 4 */
    {"column_int", 1, "ICOL"},                /* 5 */
    {"bind_blob", -2, "IVAR BLOB ?OPTIONS?"}, /* 6 */
    {"bind_parameter_count", 0, ""},          /* 7 */
    {"bind_parameter_name", 1, "IVAR"},       /* 8 */
    {"column_count", 0, ""},                  /* 9 */
    {"column_name", 1, "ICOL"},               /* 10 */
    {"column_text", 1, "ICOL"},               /* 11 */
    {"data_count", 0, ""},                    /* 12 */
    {"bind_text", -2, "IVAR TEXT ?OPTIONS?"}, /* 13 */
    {"bind_null", 1, "IVAR"},                 /* 14 */
    {"bind_parameter_index", 1, "NAME"},      /* 15 */
    {"bind_int", 2, "IVAR INTEGER"},          /* 16 */
    {"bind_int64", 2, "IVAR INTEGER"},        /* 17 */
    {"bind_text16", -3, "IVAR BYTEARRAY NBYTE"}, /* 18 */
    {"column_type", 1, "ICOL"},               /* 19 */
    {"clear_bindings", 0, ""},                /* 20 */
    {0}
  };
  int iSub = -1;
  TestStmt *p = (TestStmt*)clientData;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND ?ARGS...?");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObjStruct(
        interp, objv[1], aSub, sizeof(aSub[0]), "sub-command", 0, &iSub
  )){
    return TCL_ERROR;
  }

  if( (aSub[iSub].nArg>=0 && objc!=aSub[iSub].nArg+2)
   || (aSub[iSub].nArg<0 && objc<(aSub[iSub].nArg*-1)+2)
  ){
    Tcl_WrongNumArgs(interp, 2, objv, aSub[iSub].zUsage);
    return TCL_ERROR;
  }
  switch( iSub ){
    case 0: assert( strcmp(aSub[iSub].zSub, "step")==0 ); {
      int rc = sqlite3_step(p->pStmt);
      testSetInterpResult(interp, rc);
      break;
    };

    case 1: assert( strcmp(aSub[iSub].zSub, "finalize")==0 ); {
      int rc = sqlite3_finalize(p->pStmt);
      testSetInterpResult(interp, rc);
      p->pStmt = 0;
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      break;
    }

    case 2: assert( strcmp(aSub[iSub].zSub, "bind_double")==0 ); {
      int iVar = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      return testBindDouble(interp, p->pStmt, iVar, objv[3]);
      break;
    }

    case 3: assert( strcmp(aSub[iSub].zSub, "reset")==0 ); {
      int rc = sqlite3_reset(p->pStmt);
      testSetInterpResult(interp, rc);
      break;
    }

    case 4: assert( strcmp(aSub[iSub].zSub, "bind_zeroblob")==0 ); {
      sqlite3_int64 nByte = 0;
      int iVar = 0;
      int rc;
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      if( Tcl_GetWideIntFromObj(interp, objv[3], &nByte) ) return TCL_ERROR;
      rc = sqlite3_bind_zeroblob64(p->pStmt, iVar, nByte);
      testSetInterpResult(interp, rc);
      break;
    }

    case 5: assert( strcmp(aSub[iSub].zSub, "column_int")==0 ); {
      int iCol = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iCol) ) return TCL_ERROR;
      Tcl_SetObjResult(
          interp, Tcl_NewIntObj(sqlite3_column_int(p->pStmt, iCol))
      );
      break;
    }

    case 6: assert( strcmp(aSub[iSub].zSub, "bind_blob")==0 ); {
      int nByte = -1;
      int iVar = 0;
      const unsigned char *aBlob = 0;
      int nBlob = 0;
      int rc = SQLITE_OK;

      CmdOption aOpt[] = {
        { "-nbyte", OPT_TYPE_INTSWITCH, &nByte },
        { 0 },
      };

      assert( objc>=4 );
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      aBlob = Tcl_GetByteArrayFromObj(objv[3], &nBlob);
      if( testProcessOptions(
            interp, objc, objv, aOpt, 4, 2, "IVAR BLOB ?OPTIONS?"
      )){
        return TCL_ERROR;
      }
      if( nByte>=0 && nByte<nBlob ){
        nBlob = nByte;
      }
      rc = sqlite3_bind_blob(p->pStmt, iVar, aBlob, nBlob, SQLITE_TRANSIENT);
      testSetInterpResult(interp, rc);
      break;
    }

    case 7: assert( strcmp(aSub[iSub].zSub, "bind_parameter_count")==0 ); {
      int nRet = sqlite3_bind_parameter_count(p->pStmt);
      Tcl_SetObjResult(interp, Tcl_NewIntObj(nRet));
      break;
    }

    case 8: assert( strcmp(aSub[iSub].zSub, "bind_parameter_name")==0 ); {
      int iVar = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      Tcl_SetObjResult(interp, 
          Tcl_NewStringObj(sqlite3_bind_parameter_name(p->pStmt, iVar), -1)
      );
      break;
    }

    case 9: assert( strcmp(aSub[iSub].zSub, "column_count")==0 ); {
      int nRet = sqlite3_column_count(p->pStmt);
      Tcl_SetObjResult(interp, Tcl_NewIntObj(nRet));
      break;
    }

    case 10: assert( strcmp(aSub[iSub].zSub, "column_name")==0 ); {
      int iCol = 0;
      const char *zRet = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iCol) ) return TCL_ERROR;
      zRet = sqlite3_column_name(p->pStmt, iCol);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(zRet, -1));
      break;
    }

    case 11: assert( strcmp(aSub[iSub].zSub, "column_text")==0 ); {
      int iCol = 0;
      const char *zRet = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iCol) ) return TCL_ERROR;
      zRet = (const char*)sqlite3_column_text(p->pStmt, iCol);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(zRet, -1));
      break;
    }

    case 12: assert( strcmp(aSub[iSub].zSub, "data_count")==0 ); {
      int nRet = sqlite3_data_count(p->pStmt);
      Tcl_SetObjResult(interp, Tcl_NewIntObj(nRet));
      break;
    }
    case 13: assert( strcmp(aSub[iSub].zSub, "bind_text")==0 ); {
      int iVar = 0;
      const char *zText = 0;
      char *zFree = 0;
      int rc = SQLITE_OK;
      int nByte = -1;
      int bStatic = 0;
      int bByte = 0;

      CmdOption aOpt[] = {
        { "-nbyte", OPT_TYPE_INTSWITCH, &nByte },
        { "-static", OPT_TYPE_BOOLEAN, &bStatic },
        { "-bytearray", OPT_TYPE_BOOLEAN, &bByte },
        { 0 },
      };

      assert( objc>=4 );
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      if( testProcessOptions(
            interp, objc, objv, aOpt, 4, 2, "IVAR TEXT ?OPTIONS?") 
      ){ 
        return TCL_ERROR;
      }

      if( bByte ){
        int nActual = 0;
        zText = (const char*)Tcl_GetByteArrayFromObj(objv[3], &nActual);
        zFree = ckalloc(nActual);
        memcpy(zFree, zText, nActual);
        zFree[nActual] = 0x00;
        zText = zFree;
      }else{
        zText = Tcl_GetString(objv[3]);
      }
      if( bStatic ){
        zText = testMakeStatic(p, zText);
      }

      rc = sqlite3_bind_text(p->pStmt, iVar, zText, nByte, 
          (bStatic ? SQLITE_STATIC : SQLITE_TRANSIENT)
      );
      testSetInterpResult(interp, rc);
      ckfree(zFree);
      break;
    }

    case 14: assert( strcmp(aSub[iSub].zSub, "bind_null")==0 ); {
      int iVar = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      testSetInterpResult(interp, sqlite3_bind_null(p->pStmt, iVar));
      break;
    }

    case 15: assert( strcmp(aSub[iSub].zSub, "bind_parameter_index")==0 ); {
      const char *zName = Tcl_GetString(objv[2]);
      Tcl_SetObjResult(interp, Tcl_NewIntObj(
            sqlite3_bind_parameter_index(p->pStmt, zName)
      ));
      break;
    }

    case 16: assert( strcmp(aSub[iSub].zSub, "bind_int")==0 ); {
      int iVar = 0;
      int val = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      if( Tcl_GetIntFromObj(interp, objv[3], &val) ) return TCL_ERROR;
      testSetInterpResult(interp, sqlite3_bind_int(p->pStmt, iVar, val));
      break;
    }

    case 17: assert( strcmp(aSub[iSub].zSub, "bind_int64")==0 ); {
      int iVar = 0;
      sqlite3_int64 val = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      if( Tcl_GetWideIntFromObj(interp, objv[3], &val) ) return TCL_ERROR;
      testSetInterpResult(interp, sqlite3_bind_int64(p->pStmt, iVar, val));
      break;
    }

    case 18: assert( strcmp(aSub[iSub].zSub, "bind_text16")==0 ); {
      int iVar = 0;
      int nByte = -1;
      const unsigned char *pVal = 0;
      int nVal = 0;

      unsigned char *pFree = 0;
      int rc = SQLITE_OK;
      int bStatic = 0;

      CmdOption aOpt[] = {
        { "-static", OPT_TYPE_BOOLEAN, &bStatic },
        { 0 },
      };

      assert( objc>=5 );
      if( Tcl_GetIntFromObj(interp, objv[2], &iVar) ) return TCL_ERROR;
      if( Tcl_GetIntFromObj(interp, objv[4], &nByte) ) return TCL_ERROR;
      pVal = Tcl_GetByteArrayFromObj(objv[3], &nVal);
      if( testProcessOptions(
            interp, objc, objv, aOpt, 5, 2, "IVAR BLOB NBYTE ?OPTIONS?"
      )){
        return TCL_ERROR;
      }

      if( nByte<0 ){
        pFree = ckalloc(nVal+2);
        memcpy(pFree, pVal, nVal);
        pFree[nVal] = 0x00;
        pFree[nVal+1] = 0x00;
        pVal = pFree;
        nVal += 2;
      }
      if( bStatic ){
        pVal = testMakeStaticBlob(p, pVal, nVal);
      }

      rc = sqlite3_bind_text16(p->pStmt, iVar, pVal, nByte, 
          (bStatic ? SQLITE_STATIC : SQLITE_TRANSIENT)
      );
      testSetInterpResult(interp, rc);
      ckfree(pFree);
      break;
    }

    case 19: assert( strcmp(aSub[iSub].zSub, "column_type")==0 ); {
      const char *azType[] = {
        0, "INTEGER", "FLOAT", "TEXT", "BLOB", "NULL"
      };
      int iCol = 0;
      int eType = 0;
      if( Tcl_GetIntFromObj(interp, objv[2], &iCol) ) return TCL_ERROR;
      eType = sqlite3_column_type(p->pStmt, iCol);
      assert( eType>=1 && eType<=5 );

      Tcl_SetObjResult(interp, Tcl_NewStringObj(azType[eType], -1));
      break;
    }

    case 20: assert( strcmp(aSub[iSub].zSub, "clear_bindings")==0 ); {
      int rc = sqlite3_clear_bindings(p->pStmt);
      testSetInterpResult(interp, rc);
      break;
    }

  }

  return TCL_OK;
}

static const char *testNewStmtName(
  Tcl_Interp *interp, 
  TestPrepareContext *p, 
  char *aCmd
){
  do {
    Tcl_CmdInfo info;
    sprintf(aCmd, "stmt%d", p->iCnt++);
    if( 0==Tcl_GetCommandInfo(interp, aCmd, &info) ) break;
  }while( 1 );
  return aCmd;
}

/*
** Create a statement object.
**
**   sqlite3_prepare ?OPTIONS? CMDNAME DB SQL
*/
static int SQLITE_TCLAPI test_sqlite3_prepare(
  ClientData clientData, /* Pointer to sqlite3_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  TestPrepareContext *p = (TestPrepareContext*)clientData;
  sqlite3 *db = 0;
  int rc = SQLITE_OK;
  sqlite3_stmt *pStmt = 0;
  TestStmt *pRet = 0;

  const char *zCmd = 0;
  const char *zDb = 0;
  const char *zSql = 0;
  int nByte = -1;
  int bV2 = 0;
  int bV3 = 0;
  const char *zUsage = "?OPTIONS? ?CMDNAME? DB SQL";
  char aCmd[32];
  const char *zTailvar = 0;
  const char *zTail = 0;

  CmdOption aOpt[] = {
    { "",       OPT_TYPE_POSITION,  (void*)&zCmd },
    { "",       OPT_TYPE_POSITION,  (void*)&zDb },
    { "",       OPT_TYPE_POSITION,  (void*)&zSql },
    { "-v2",    OPT_TYPE_BOOLEAN,   (void*)&bV2 },
    { "-v3",    OPT_TYPE_BOOLEAN,   (void*)&bV3 },
    { "-nbyte", OPT_TYPE_INTSWITCH, (void*)&nByte },
    { "-tail",  OPT_TYPE_STRSWITCH, (void*)&zTailvar },
    { 0 },
  };

  rc = testProcessOptions(interp, objc, objv, aOpt, 1, 1, zUsage);
  if( rc ){
    return rc;
  }
  if( zDb==0 ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(zUsage, -1));
    return TCL_ERROR;
  }
  if( zSql==0 ){
    zSql = zDb;
    zDb = zCmd;
    zCmd = testNewStmtName(interp, p, aCmd);
  }

  if( getDbPointer(interp, zDb, &db) ) return TCL_ERROR;

  rc = sqlite3_prepare(db, zSql, nByte, &pStmt, &zTail);
  if( rc!=SQLITE_OK ){
    Tcl_AppendResult(interp, sqlite3_errmsg(db), (char*)0);
    return TCL_ERROR;
  }
  if( zTailvar ){
    Tcl_SetVar(interp, zTailvar, zTail, 0);
  }

  pRet = (TestStmt*)ckalloc(sizeof(TestStmt));
  memset(pRet, 0, sizeof(TestStmt));
  pRet->pStmt = pStmt;
  Tcl_CreateObjCommand(interp, zCmd, test_stmt_cmd, (void*)pRet, del_stmt_cmd);
  Tcl_SetObjResult(interp, Tcl_NewStringObj(zCmd, -1));
  return TCL_OK;
}

/*
** Tclcmd:
**
**   btree_is_memdb DB DBNAME
**
** DB is a database handle command, and IDB must be an integer. This command
** returns "memory", "disk" or "unknown", if b-tree IDB is currently in memory,
** on disk or not opened.
*/
static int SQLITE_TCLAPI test_btree_is_memdb(
  ClientData clientData, /* Pointer to sqlite3_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  sqlite3 *db = 0;
  const char *zName = 0;
  sqlite3_file *pFile = 0;
  const char *zRes = 0;
 
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB IDB");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  zName = Tcl_GetString(objv[2]);

  sqlite3_file_control(db, zName, SQLITE_FCNTL_FILE_POINTER, &pFile);
  if( pFile==0 ){
    zRes = "unknown";
  }else if( pFile->pMethods==0 ){
    zRes = "memory";
  }else{
    zRes = "disk";
  }

  Tcl_SetObjResult(interp, Tcl_NewStringObj(zRes, -1));
  return TCL_OK;
}

static void test_free_ctx(ClientData clientData){
  ckfree(clientData);
}

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest_stmt_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
  } aObjCmd[] = {
     { "sqlite3_prepare", test_sqlite3_prepare },
     { "btree_is_memdb", test_btree_is_memdb },
  };
  int i;
  TestPrepareContext *p = 0;

  p = (TestPrepareContext*)ckalloc(sizeof(TestPrepareContext));
  p->iCnt = 0;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(
        interp, aObjCmd[i].zName, aObjCmd[i].xProc, (ClientData)p, test_free_ctx
    );
    p = 0;
  }

  return TCL_OK;
}
