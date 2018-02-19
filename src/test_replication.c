/*
** 2018 February 19
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
** This file contains code used for testing the SQLite system.
** None of the code in this file goes into a deliverable build.
**
** This file contains a stub implementation of the write-ahead log replication
** interface. It can be used by tests to exercise the replication APIs exposed
** by SQLite.
**
** This replication implementation is designed for testability and does
** not involve any actual networking.
*/
#if defined(INCLUDE_SQLITE_TCL_H)
#  include "sqlite_tcl.h"
#else
#  include "tcl.h"
#endif

#if defined(SQLITE_ENABLE_REPLICATION) && !defined(SQLITE_OMIT_WAL)

#include "sqliteInt.h"
#include "sqlite3.h"
#include <assert.h>

extern const char *sqlite3ErrName(int);

/* These functions are implemented in test1.c. */
extern int getDbPointer(Tcl_Interp *, const char *, sqlite3 **);

/*
** A no-op version of sqlite3_replication_methods.xBegin().
*/
static int testreplicationBegin(void *pCtx){
  return 0;
}

/*
** A no-op version of sqlite3_replication_methods.xAbort().
*/
static int testreplicationAbort(void *pCtx){
  return 0;
}

/*
** A no-op version of sqlite3_replication_methods.xFrames().
*/
static int testreplicationFrames(void *pCtx, int szPage, int nList,
  sqlite3_replication_page *pList, unsigned nTruncate, int isCommit,
  unsigned sync_flags
){
  return 0;
}

/*
** A no-op version of sqlite3_replication_methods.xUndo().
*/
static int testreplicationUndo(void *pCtx){
  return 0;
}

/*
** A no-op version of sqlite3_replication_methods.xEnd().
*/
static int testreplicationEnd(void *pCtx){
  return 0;
}

/*
** Invoke this routine to register or unregister the stub write-ahead log
** replication implemented by this file.
**
** Install the test replication if installFlag is 1 and uninstall it if
** installFlag is 0.
*/
void installTestReplication(
  int installFlag            /* True to install.  False to uninstall. */
){
  static const sqlite3_replication_methods testReplication = {
    testreplicationBegin,
    testreplicationAbort,
    testreplicationFrames,
    testreplicationUndo,
    testreplicationEnd,
  };
  static sqlite3_replication_methods defaultReplication;
  static int isInstalled = 0;

  if( installFlag!=isInstalled ){
    if( installFlag ){
      sqlite3_config(SQLITE_CONFIG_GETREPLICATION, &defaultReplication);
      assert( defaultReplication.xBegin==0 );
      sqlite3_config(SQLITE_CONFIG_REPLICATION, &testReplication);
    }else{
      assert( defaultReplication.xBegin==0 );
      sqlite3_config(SQLITE_CONFIG_REPLICATION, &defaultReplication);
    }
    isInstalled = installFlag;
  }
}

/*
** Usage:    sqlite3_config_test_replication INSTALL_FLAG
**
** Set up the test write-ahead log replication.  Install if INSTALL_FLAG is true
** and uninstall (reverting to no replication at all) if INSTALL_FLAG is false.
*/
static int SQLITE_TCLAPI test_replication(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int installFlag;
  extern void installTestReplication(int);
  if( objc<2 || objc>2 ){
    Tcl_WrongNumArgs(interp, 1, objv,
        "INSTALLFLAG");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &installFlag) ) return TCL_ERROR;
  installTestReplication(installFlag);

  return TCL_OK;
}

/*
** Usage:    sqlite3_replication_mode HANDLE SCHEMA
**
** Return the the name of the replication mode value of the given database.
*/
static int SQLITE_TCLAPI test_replication_mode(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  sqlite3 *db;
  const char *zSchema;
  int mode;
  int iDb;
  Btree *pBt;
  Pager *pPager;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv,
        "HANDLE SCHEMA");
    return TCL_ERROR;
  }

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ){
    return TCL_ERROR;
  }
  zSchema = Tcl_GetString(objv[2]);

  rc = sqlite3_replication_mode(db, zSchema, &mode);

  if( rc!=SQLITE_OK ){
    Tcl_AppendResult(interp, sqlite3ErrName(rc), (char*)0);
    return TCL_ERROR;
  }

  const char *zMode = 0;
  switch( mode ){
    case SQLITE_REPLICATION_NONE:  zMode = "SQLITE_REPLICATION_NONE";    break;
    case SQLITE_REPLICATION_LEADER:  zMode = "SQLITE_REPLICATION_LEADER";    break;
    case SQLITE_REPLICATION_FOLLOWER:  zMode = "SQLITE_REPLICATION_FOLLOWER";    break;
  }
  if( zMode==0 ){
    static char zBuf[50];
    sqlite3_snprintf(sizeof(zBuf), zBuf, "SQLITE_REPLICATION_UNKNOWN(%d)", mode);
    zMode = zBuf;
  }

  Tcl_AppendResult(interp, zMode, (char*)0);
  return TCL_OK;
}

#endif  /* SQLITE_ENABLE_REPLICATION */

/*
** This routine registers the custom TCL commands defined in this
** module.  This should be the only procedure visible from outside
** of this module.
*/
int Sqlitetestreplication_Init(Tcl_Interp *interp){
#if defined(SQLITE_ENABLE_REPLICATION) && !defined(SQLITE_OMIT_WAL)
  Tcl_CreateObjCommand(interp, "sqlite3_config_test_replication",
          test_replication,0,0);
  Tcl_CreateObjCommand(interp, "sqlite3_replication_mode",
          test_replication_mode, 0, 0);
#endif  /* SQLITE_ENABLE_REPLICATION */
  return TCL_OK;
}
