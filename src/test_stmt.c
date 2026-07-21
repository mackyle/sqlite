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

typedef struct TestStmt TestStmt;
struct TestStmt {
  sqlite3_stmt *pStmt;
};

extern const char *sqlite3ErrName(int);
extern int getDbPointer(Tcl_Interp *, const char *, sqlite3 **);

/*
** Set the result of the interpreter passed as the first argument to the
** string representation of the SQLite error code passed as the second.
*/
static void testSetInterpResult(Tcl_Interp *interp, int rc){
  Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
}

static void del_stmt_cmd(ClientData clientData){
  TestStmt *p = (TestStmt*)clientData;
  sqlite3_finalize(p->pStmt);
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
    {"bind_blob", -1, "iVAR BLOB ?OPTIONS?"}, /* 6 */
    {0, 0}
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
  if( aSub[iSub].nArg>=0 && objc!=aSub[iSub].nArg+2 ){
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
      break;
    }
  }

  return TCL_OK;
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
  sqlite3 *db = 0;
  int rc = SQLITE_OK;
  const char *zCmd = 0;
  const char *zDb = 0;
  const char *zSql = 0;
  sqlite3_stmt *pStmt = 0;
  TestStmt *pRet = 0;
  int ii;
  int nByte = -1;

  if( objc<4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?OPTIONS? CMDNAME DB SQL");
    return TCL_ERROR;
  }

  for(ii=1; ii<objc; ii++){
    char *zArg = Tcl_GetString(objv[ii]);
    if( zArg[0]!='-' ){
      if( zCmd==0 ) zCmd = zArg;
      else if( zDb==0 ) zDb = zArg;
      else if( zSql==0 ) zSql = zArg;
      else goto usage;
    }else{
      const char *azOpt[] = {
        "-v2",     /* 0 */
        "-v3",     /* 1 */
        "-nbyte",  /* 2 */
        0
      };
      int iOpt = -1;
      if( Tcl_GetIndexFromObj(interp, objv[ii], azOpt, "option", 0, &iOpt) ){
        return TCL_ERROR;
      }
      if( iOpt==2 && ii==(objc-1) ){
        Tcl_AppendResult(
            interp, "option requires an argument: ", Tcl_GetString(objv[ii]),
            (char*)0
        );
        return TCL_ERROR;
      }
      switch( iOpt ){
        case 2: assert( strcmp("-nbyte", azOpt[iOpt])==0 ); {
          if( Tcl_GetIntFromObj(interp, objv[ii+1], &nByte) ){
             return TCL_ERROR;
          }
          ii++;
          break;
        }
      }
    }
  }
  if( zCmd==0 || zDb==0 || zSql==0 ) goto usage;

  if( getDbPointer(interp, zDb, &db) ) return TCL_ERROR;

  rc = sqlite3_prepare(db, zSql, nByte, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    Tcl_AppendResult(interp, sqlite3_errmsg(db), (char*)0);
    return TCL_ERROR;
  }

  pRet = (TestStmt*)ckalloc(sizeof(TestStmt));
  pRet->pStmt = pStmt;
  Tcl_CreateObjCommand(interp, zCmd, test_stmt_cmd, (void*)pRet, del_stmt_cmd);
  Tcl_SetObjResult(interp, Tcl_NewStringObj(zCmd, -1));
  return TCL_OK;

 usage:
  Tcl_AppendResult(
      interp, Tcl_GetString(objv[0]), " ?OPTIONS? CMDNAME DB SQL", (char*)0
  );
  return TCL_ERROR;
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
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc, 0, 0);
  }

  return TCL_OK;
}
