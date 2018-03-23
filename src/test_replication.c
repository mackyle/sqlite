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
  int eFailing;        /* Code of a method that should fail when triggered */
  int rc;              /* If non-zero, the eFailing method will error */
  int n;               /* Number of times the eFailing method will error */
};
static testreplicationContextGlobalType testreplicationContextGlobal;

#define STATE_IDLE      0
#define STATE_PENDING   1
#define STATE_WRITING   2
#define STATE_COMMITTED 3
#define STATE_UNDONE    4

#define FAILING_BEGIN  1
#define FAILING_FRAMES 2
#define FAILING_UNDO   3
#define FAILING_END    4

/*
** A version of sqlite3_replication_methods.xBegin() that transitions the global
** replication context state to STATE_PENDING.
*/
static int testreplicationBegin(void *pCtx){
  int rc = SQLITE_OK;
  assert( pCtx==&testreplicationContextGlobal );
  assert( testreplicationContextGlobal.eState==STATE_IDLE );
  testreplicationContextGlobal.eState = STATE_PENDING;
  if( testreplicationContextGlobal.n > 0
   && testreplicationContextGlobal.eFailing==FAILING_BEGIN ){
    rc = testreplicationContextGlobal.rc;
    testreplicationContextGlobal.n--;
    /* Switch back to IDLE, since xEnd won't be invoked */
    testreplicationContextGlobal.eState = STATE_IDLE;
  }
  return rc;
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
  if( testreplicationContextGlobal.n > 0
   && testreplicationContextGlobal.eFailing==FAILING_FRAMES ){
    rc = testreplicationContextGlobal.rc;
    testreplicationContextGlobal.n--;
  }else if( testreplicationContextGlobal.db ){
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
  if( testreplicationContextGlobal.n > 0
   && testreplicationContextGlobal.eFailing==FAILING_UNDO ){
    rc = testreplicationContextGlobal.rc;
    testreplicationContextGlobal.n--;
  }else if( testreplicationContextGlobal.db
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
  int rc = SQLITE_OK;
  assert( pCtx==&testreplicationContextGlobal );
  assert( testreplicationContextGlobal.eState==STATE_PENDING
       || testreplicationContextGlobal.eState==STATE_COMMITTED
       || testreplicationContextGlobal.eState==STATE_UNDONE
  );
  testreplicationContextGlobal.eState = STATE_IDLE;
  if( testreplicationContextGlobal.n > 0
   && testreplicationContextGlobal.eFailing==FAILING_END ){
    rc = testreplicationContextGlobal.rc;
    testreplicationContextGlobal.n--;
  }
  return rc;
}

/*
** Invoke this routine to register or unregister the stub write-ahead log
** replication implemented by this file.
**
** Install the test replication if installFlag is 1 and uninstall it if
** installFlag is 0.
**
** If hook
*/
void installTestReplication(
  int installFlag,            /* True to install.  False to uninstall. */
  int eFailing,               /* Code of a method that will fail. */
  int rc,                     /* Error that a failing method will return. */
  int n                       /* Number of subsequent failures of the failing. */
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
      testreplicationContextGlobal.eFailing = eFailing;
      testreplicationContextGlobal.rc = rc;
      testreplicationContextGlobal.n = n;

      sqlite3_config(SQLITE_CONFIG_REPLICATION, &testReplication);
    }else{
      assert( defaultReplication.xBegin==0 );
      sqlite3_config(SQLITE_CONFIG_REPLICATION, &defaultReplication);
    }
    isInstalled = installFlag;
  }
}

/*
** Usage:    sqlite3_config_test_replication INSTALL_FLAG [FAILING_METHOD ERROR [N]]
**
** Set up the test write-ahead log replication.  Install if INSTALL_FLAG is true
** and uninstall (reverting to no replication at all) if INSTALL_FLAG is false.
**
** If FAILING_METHOD is given (either "xBegin", "xFrames" or "xEnd"), then that
** method will fail returning ERROR (either "NOT_LEADER" or
** "LEADERSHIP_LOST"). If N is also given, the method will fail only N times,
** and the N+1 invokation will be successful.
*/
static int SQLITE_TCLAPI test_replication(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int installFlag;
  const char *zFailing;
  const char *zError;
  int eFailing;
  int rc;
  int n = 8192; // Set an effectively "infinite" number of failures by default.
  extern void installTestReplication(int, int, int, int);
  if( objc<2 || objc==3 || objc>5 ){
    Tcl_WrongNumArgs(interp, 1, objv,
        "INSTALL_FLAG [FAILING_METHOD RC]");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &installFlag) ) return TCL_ERROR;
  if( objc>=3 ){
    /* Failing method */
    zFailing = Tcl_GetString(objv[2]);
    if( strcmp(zFailing, "xBegin")==0 ){
      eFailing = FAILING_BEGIN;
    }else if( strcmp(zFailing, "xFrames")==0 ){
      eFailing = FAILING_FRAMES;
    }else if( strcmp(zFailing, "xUndo")==0 ){
      eFailing = FAILING_UNDO;
    }else if( strcmp(zFailing, "xEnd")==0 ){
      eFailing = FAILING_END;
    }else{
      Tcl_AppendResult(interp, "unknown replication method", (char*)0);
      return TCL_ERROR;
    }

    /* Error code */
    zError = Tcl_GetString(objv[3]);
    if( strcmp(zError, "NOT_LEADER")==0 ){
      rc = SQLITE_IOERR_NOT_LEADER;
    }else if( strcmp(zError, "LEADERSHIP_LOST")==0 ){
      rc = SQLITE_IOERR_LEADERSHIP_LOST;
    }else{
      Tcl_AppendResult(interp, "unknown error", (char*)0);
      return TCL_ERROR;
    }

    /* Number of subsequent failures */
    if( objc>=5 ){
      if( Tcl_GetIntFromObj(interp, objv[4], &n) ) return TCL_ERROR;
    }
  }

  installTestReplication(installFlag, eFailing, rc, n);

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

/*
** Usage:    sqlite3_replication_checkpoint HANDLE SCHEMA
**
** Checkpoint a database in follower replication mode, using the
** SQLITE_CHECKPOINT_TRUNCATE checkpoint mode.
*/
static int SQLITE_TCLAPI test_replication_checkpoint(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  sqlite3 *db;
  const char *zSchema;
  int nLog;
  int nCkpt;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv,
        "HANDLE SCHEMA");
    return TCL_ERROR;
  }

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ){
    return TCL_ERROR;
  }
  zSchema = Tcl_GetString(objv[2]);

  rc = sqlite3_replication_checkpoint(db, zSchema,
      SQLITE_CHECKPOINT_TRUNCATE, &nLog, &nCkpt);

  if( rc!=SQLITE_OK ){
    Tcl_AppendResult(interp, sqlite3ErrName(rc), (char*)0);
    return TCL_ERROR;
  }
  if( nLog!=0 ){
    Tcl_AppendResult(interp, "the WAL was not truncated", (char*)0);
    return TCL_ERROR;
  }
  if( nCkpt!=0 ){
    Tcl_AppendResult(interp, "only some frames were checkpointed", (char*)0);
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
  Tcl_CreateObjCommand(interp, "sqlite3_replication_checkpoint",
          test_replication_checkpoint, 0, 0);
#endif  /* SQLITE_ENABLE_REPLICATION */
  return TCL_OK;
}
