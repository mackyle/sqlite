/*
** 2015 February 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/

#include "sqlite3.h"

#if defined(SQLITE_TEST)
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_RBU)

#include "sqlite3rbu.h"
#include <tcl.h>
#include <assert.h>

/* From main.c (apparently...) */
extern const char *sqlite3ErrName(int);

void test_rbu_delta(sqlite3_context *pCtx, int nArg, sqlite3_value **apVal){
  Tcl_Interp *interp = (Tcl_Interp*)sqlite3_user_data(pCtx);
  Tcl_Obj *pScript;
  int i;

  pScript = Tcl_NewObj();
  Tcl_IncrRefCount(pScript);
  Tcl_ListObjAppendElement(0, pScript, Tcl_NewStringObj("rbu_delta", -1));
  for(i=0; i<nArg; i++){
    sqlite3_value *pIn = apVal[i];
    const char *z = (const char*)sqlite3_value_text(pIn);
    Tcl_ListObjAppendElement(0, pScript, Tcl_NewStringObj(z, -1));
  }

  if( TCL_OK==Tcl_EvalObjEx(interp, pScript, TCL_GLOBAL_ONLY) ){
    const char *z = Tcl_GetStringResult(interp);
    sqlite3_result_text(pCtx, z, -1, SQLITE_TRANSIENT);
  }else{
    Tcl_BackgroundError(interp);
  }

  Tcl_DecrRefCount(pScript);
}


static int test_sqlite3rbu_cmd(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int ret = TCL_OK;
  sqlite3rbu *pRbu = (sqlite3rbu*)clientData;
  const char *azMethod[] = { 
    "step", "close", "create_rbu_delta", "savestate", 0 
  };
  int iMethod;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "METHOD");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObj(interp, objv[1], azMethod, "method", 0, &iMethod) ){
    return TCL_ERROR;
  }

  switch( iMethod ){
    case 0: /* step */ {
      int rc = sqlite3rbu_step(pRbu);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
      break;
    }

    case 1: /* close */ {
      char *zErrmsg = 0;
      int rc;
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      rc = sqlite3rbu_close(pRbu, &zErrmsg);
      if( rc==SQLITE_OK || rc==SQLITE_DONE ){
        Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
        assert( zErrmsg==0 );
      }else{
        Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
        if( zErrmsg ){
          Tcl_AppendResult(interp, " - ", zErrmsg, 0);
          sqlite3_free(zErrmsg);
        }
        ret = TCL_ERROR;
      }
      break;
    }

    case 2: /* create_rbu_delta */ {
      sqlite3 *db = sqlite3rbu_db(pRbu, 0);
      int rc = sqlite3_create_function(
          db, "rbu_delta", -1, SQLITE_UTF8, (void*)interp, test_rbu_delta, 0, 0
      );
      Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
      ret = (rc==SQLITE_OK ? TCL_OK : TCL_ERROR);
      break;
    }

    case 3: /* savestate */ {
      int rc = sqlite3rbu_savestate(pRbu);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
      ret = (rc==SQLITE_OK ? TCL_OK : TCL_ERROR);
      break;
    }

    default: /* seems unlikely */
      assert( !"cannot happen" );
      break;
  }

  return ret;
}

/*
** Tclcmd: sqlite3rbu CMD <target-db> <rbu-db> ?<state-db>?
*/
static int test_sqlite3rbu(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  sqlite3rbu *pRbu = 0;
  const char *zCmd;
  const char *zTarget;
  const char *zRbu;
  const char *zStateDb = 0;

  if( objc!=4 && objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME TARGET-DB RBU-DB ?STATE-DB?");
    return TCL_ERROR;
  }
  zCmd = Tcl_GetString(objv[1]);
  zTarget = Tcl_GetString(objv[2]);
  zRbu = Tcl_GetString(objv[3]);
  if( objc==5 ) zStateDb = Tcl_GetString(objv[4]);

  pRbu = sqlite3rbu_open(zTarget, zRbu, zStateDb);
  Tcl_CreateObjCommand(interp, zCmd, test_sqlite3rbu_cmd, (ClientData)pRbu, 0);
  Tcl_SetObjResult(interp, objv[1]);
  return TCL_OK;
}

/*
** Tclcmd: sqlite3rbu_create_vfs ?-default? NAME PARENT
*/
static int test_sqlite3rbu_create_vfs(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *zName;
  const char *zParent;
  int rc;

  if( objc!=3 && objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-default? NAME PARENT");
    return TCL_ERROR;
  }

  zName = Tcl_GetString(objv[objc-2]);
  zParent = Tcl_GetString(objv[objc-1]);
  if( zParent[0]=='\0' ) zParent = 0;

  rc = sqlite3rbu_create_vfs(zName, zParent);
  if( rc!=SQLITE_OK ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3ErrName(rc), -1));
    return TCL_ERROR;
  }else if( objc==4 ){
    sqlite3_vfs *pVfs = sqlite3_vfs_find(zName);
    sqlite3_vfs_register(pVfs, 1);
  }

  Tcl_ResetResult(interp);
  return TCL_OK;
}

/*
** Tclcmd: sqlite3rbu_destroy_vfs NAME
*/
static int test_sqlite3rbu_destroy_vfs(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *zName;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME");
    return TCL_ERROR;
  }

  zName = Tcl_GetString(objv[1]);
  sqlite3rbu_destroy_vfs(zName);
  return TCL_OK;
}

/*
** Tclcmd: sqlite3rbu_internal_test
*/
static int test_sqlite3rbu_internal_test(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  sqlite3 *db;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  db = sqlite3rbu_db(0, 0);
  if( db!=0 ){
    Tcl_AppendResult(interp, "sqlite3rbu_db(0, 0)!=0", 0);
    return TCL_ERROR;
  }

  return TCL_OK;
}

int SqliteRbu_Init(Tcl_Interp *interp){ 
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
  } aObjCmd[] = {
    { "sqlite3rbu", test_sqlite3rbu },
    { "sqlite3rbu_create_vfs", test_sqlite3rbu_create_vfs },
    { "sqlite3rbu_destroy_vfs", test_sqlite3rbu_destroy_vfs },
    { "sqlite3rbu_internal_test", test_sqlite3rbu_internal_test },
  };
  int i;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc, 0, 0);
  }
  return TCL_OK;
}

#else
#include <tcl.h>
int SqliteRbu_Init(Tcl_Interp *interp){ return TCL_OK; }
#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_RBU) */
#endif /* defined(SQLITE_TEST) */
