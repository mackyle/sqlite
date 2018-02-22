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
** Global replication context used by this stub implementation of
** sqlite3_replication_methods. It holds a pointer to a connection in follower
** replication mode.
*/
typedef struct testreplicationContextGlobalType testreplicationContextGlobalType;
struct testreplicationContextGlobalType {
  sqlite3 *db;         /* Follower connection */
  const char *zSchema; /* Follower schema name */
  int eState;          /* Replication state (IDLE, PENDING, WRITING, etc) */
};
static testreplicationContextGlobalType testreplicationContextGlobal;

#define STATE_IDLE      0
#define STATE_PENDING   1
#define STATE_WRITING   2
#define STATE_COMMITTED 3
#define STATE_UNDONE    4

/*
** A version of sqlite3_replication_methods.xBegin() that transitions the global
** replication context state to STATE_PENDING.
*/
static int testreplicationBegin(void *pCtx){
  assert( pCtx==&testreplicationContextGlobal );
  assert( testreplicationContextGlobal.eState==STATE_IDLE );
  testreplicationContextGlobal.eState = STATE_PENDING;
  return 0;
}

/*
** A version of sqlite3_replication_methods.xAbort() that transitions the global
** replication context state to STATE_IDLE.
*/
static int testreplicationAbort(void *pCtx){
  assert( testreplicationContextGlobal.eState==STATE_PENDING );
  testreplicationContextGlobal.eState = STATE_IDLE;
  return 0;
}

/*
** A version of sqlite3_replication_methods.xFrames() that invokes
** sqlite3_replication_frames() on the follower connection configured in the
** global test replication context (if present).
*/
static int testreplicationFrames(void *pCtx, int szPage, int nList,
  sqlite3_replication_page *pList, unsigned nTruncate, int isCommit,
  unsigned sync_flags
){
  int rc = SQLITE_OK;
  int isBegin;
  assert( pCtx==&testreplicationContextGlobal );
  assert( testreplicationContextGlobal.eState==STATE_PENDING
       || testreplicationContextGlobal.eState==STATE_WRITING
  );
  if( testreplicationContextGlobal.db ){
    /* If the replication state is STATE_PENDING, it means that this is the
    ** first batch of frames of a new transaction. */
    if( testreplicationContextGlobal.eState==STATE_PENDING ){
      isBegin = 1;
      testreplicationContextGlobal.eState = STATE_WRITING;
    }
    rc = sqlite3_replication_frames(
          testreplicationContextGlobal.db,
          testreplicationContextGlobal.zSchema,
          isBegin, szPage, nList, pList, nTruncate, isCommit, sync_flags
    );
    if( isCommit ){
      testreplicationContextGlobal.eState = STATE_COMMITTED;
    }
  }
  return rc;
}

/*
** A version of sqlite3_replication_methods.xUndo() that invokes
** sqlite3_replication_undo() on the follower connection configured in the
** global test replication context (if present).
*/
static int testreplicationUndo(void *pCtx){
  int rc = SQLITE_OK;
  assert( pCtx==&testreplicationContextGlobal );
  assert( testreplicationContextGlobal.eState==STATE_PENDING
       || testreplicationContextGlobal.eState==STATE_WRITING
  );
  if( testreplicationContextGlobal.db
   && testreplicationContextGlobal.eState==STATE_WRITING ){
    rc = sqlite3_replication_undo(
        testreplicationContextGlobal.db,
        testreplicationContextGlobal.zSchema
    );
    testreplicationContextGlobal.eState = STATE_UNDONE;
  }
  return rc;
}

/*
** A version of sqlite3_replication_methods.xEnd() that transitions the global
** replication context state to STATE_IDLE.
*/
static int testreplicationEnd(void *pCtx){
  assert( pCtx==&testreplicationContextGlobal );
  assert( testreplicationContextGlobal.eState==STATE_PENDING
       || testreplicationContextGlobal.eState==STATE_COMMITTED
       || testreplicationContextGlobal.eState==STATE_UNDONE
  );
  testreplicationContextGlobal.eState = STATE_IDLE;
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

      /* Reset any previous state */
      testreplicationContextGlobal.db = 0;
      testreplicationContextGlobal.zSchema = 0;
      testreplicationContextGlobal.eState = STATE_IDLE;

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

/*
** Usage:    sqlite3_replication_leader HANDLE SCHEMA
**
** Enable leader replication for the given connection/schema.
*/
static int SQLITE_TCLAPI test_replication_leader(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  sqlite3 *db;
  const char *zSchema;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv,
        "HANDLE SCHEMA");
    return TCL_ERROR;
  }

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ){
    return TCL_ERROR;
  }
  zSchema = Tcl_GetString(objv[2]);

  rc = sqlite3_replication_leader(db, zSchema, &testreplicationContextGlobal);

  if( rc!=SQLITE_OK ){
    Tcl_AppendResult(interp, sqlite3ErrName(rc), (char*)0);
    return TCL_ERROR;
  }

  return TCL_OK;
}

/*
** Usage:    sqlite3_replication_follower HANDLE SCHEMA
**
** Enable follower replication for the given connection/schema. The global test
** replication context will be set to point to this connection/schema and WAL
** events will be replicated to it.
*/
static int SQLITE_TCLAPI test_replication_follower(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  sqlite3 *db;
  const char *zSchema;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv,
        "HANDLE SCHEMA");
    return TCL_ERROR;
  }

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ){
    return TCL_ERROR;
  }
  zSchema = Tcl_GetString(objv[2]);

  rc = sqlite3_replication_follower(db, zSchema);

  if( rc!=SQLITE_OK ){
    Tcl_AppendResult(interp, sqlite3ErrName(rc), (char*)0);
    return TCL_ERROR;
  }

  testreplicationContextGlobal.db = db;
  testreplicationContextGlobal.zSchema = zSchema;

  return TCL_OK;
}

/*
** Usage:    sqlite3_replication_none HANDLE SCHEMA
**
** Disable leader or follower replication for the given connection/schema.
*/
static int SQLITE_TCLAPI test_replication_none(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  sqlite3 *db;
  const char *zSchema;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv,
        "HANDLE SCHEMA");
    return TCL_ERROR;
  }

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ){
    return TCL_ERROR;
  }
  zSchema = Tcl_GetString(objv[2]);

  rc = sqlite3_replication_none(db, zSchema);

  if( rc!=SQLITE_OK ){
    Tcl_AppendResult(interp, sqlite3ErrName(rc), (char*)0);
    return TCL_ERROR;
  }

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
  Tcl_CreateObjCommand(interp, "sqlite3_replication_leader",
          test_replication_leader, 0, 0);
  Tcl_CreateObjCommand(interp, "sqlite3_replication_follower",
          test_replication_follower, 0, 0);
  Tcl_CreateObjCommand(interp, "sqlite3_replication_none",
          test_replication_none, 0, 0);
#endif  /* SQLITE_ENABLE_REPLICATION */
  return TCL_OK;
}
