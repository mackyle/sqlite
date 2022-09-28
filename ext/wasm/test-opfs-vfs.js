/*
  2022-09-17

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  A testing ground for the OPFS VFS.

  Summary of how this works:

  This file uses the sqlite3.StructBinder-created struct wrappers for
  sqlite3_vfs, sqlite3_io_methods, ans sqlite3_file to set up a
  conventional sqlite3_vfs (except that it's implemented in JS). The
  methods which require OPFS APIs use a separate worker (hereafter called the
  OPFS worker) to access that functionality. This worker and that one
  use SharedArrayBuffer.
*/
'use strict';
const tryOpfsVfs = function(sqlite3){
  const toss = function(...args){throw new Error(args.join(' '))};
  const logPrefix = "OPFS tester:";
  const log = (...args)=>console.log(logPrefix,...args);
  const warn =  (...args)=>console.warn(logPrefix,...args);
  const error =  (...args)=>console.error(logPrefix,...args);
  log("tryOpfsVfs()");
  const capi = sqlite3.capi;
  const pVfs = capi.sqlite3_vfs_find("opfs") || toss("Missing 'opfs' VFS.");
  const oVfs = capi.sqlite3_vfs.instanceForPointer(pVfs) || toss("Unexpected instanceForPointer() result.");;
  log("OPFS VFS:",pVfs, oVfs);

  const urlArgs = new URL(self.location.href).searchParams;
  const dbFile = "my-persistent.db";
  if(urlArgs.has('delete')) sqlite3.opfs.deleteEntry(dbFile);

  const opfs = sqlite3.opfs;
  const db = new opfs.OpfsDb(dbFile);
  log("db file:",db.filename);
  try{
    if(opfs.entryExists(dbFile)){
      let n = db.selectValue("select count(*) from sqlite_schema");
      log("Persistent data found. sqlite_schema entry count =",n);
    }
    db.transaction((db)=>{
      db.exec({
        sql:[
          "create table if not exists t(a);",
          "insert into t(a) values(?),(?),(?);",
        ],
        bind: [performance.now() | 0,
               (performance.now() |0) / 2,
               (performance.now() |0) / 4]
      });
    });
    log("count(*) from t =",db.selectValue("select count(*) from t"));

    // Some sanity checks of the opfs utility functions...
    const testDir = '/sqlite3-opfs-'+opfs.randomFilename(12);
    const aDir = testDir+'/test/dir';
    opfs.mkdir(aDir) || toss("mkdir failed");
    opfs.mkdir(aDir) || toss("mkdir must pass if the dir exists");
    opfs.deleteEntry(testDir+'/test') && toss("delete 1 should have failed (dir not empty)");
    opfs.deleteEntry(testDir+'/test/dir') || toss("delete 2 failed");
    opfs.deleteEntry(testDir+'/test/dir') && toss("delete 2b should have failed (dir already deleted)");
    opfs.deleteEntry(testDir,true) || toss("delete 3 failed");
    opfs.entryExists(testDir) && toss("entryExists(",testDir,") should have failed");
  }finally{
    db.close();
  }

  log("Done!");
}/*tryOpfsVfs()*/;

importScripts('sqlite3.js');
self.sqlite3InitModule()
  .then((EmscriptenModule)=>EmscriptenModule.sqlite3.asyncPostInit())
  .then((sqlite3)=>tryOpfsVfs(sqlite3))
  .catch((e)=>{
    console.error("Error initializing module:",e);
  });