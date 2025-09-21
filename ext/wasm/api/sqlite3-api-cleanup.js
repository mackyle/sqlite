/*
  2022-07-22

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  This file is the tail end of the sqlite3-api.js constellation,
  intended to be appended after all other sqlite3-api-*.js files so
  that it can finalize any setup and clean up any global symbols
  temporarily used for setting up the API's various subsystems.

  In Emscripten builds it's run in the context of a Module.postRun
  handler.
*/
'use strict';
if('undefined' !== typeof Module){ // presumably an Emscripten build
  /**
     The WASM-environment-specific configuration pieces
     for sqlite3ApiBootstrap().
  */
  const SABC = Object.assign(
    Object.create(null),
    globalThis.sqlite3ApiConfig || {}, {
      exports: ('undefined'===typeof wasmExports)
        ? Module['asm']/* emscripten <=3.1.43 */
        : wasmExports  /* emscripten >=3.1.44 */,
      memory: Module.wasmMemory /* gets set if built with -sIMPORTED_MEMORY */
    }
  );

  /** Figure out if this is a 32- or 64-bit WASM build. */
  switch( typeof SABC.exports.sqlite3_libversion() ){
    case 'number':
      SABC.wasmPtrIR = 'i32';
      SABC.wasmPtrSize = 4;
      break;
    case 'bigint':
      SABC.wasmPtrIR = 'i64';
      SABC.wasmPtrSize = 8;
      break;
    default:
      throw new Error("Cannot determine whether this is a 32- or 64-bit build");
  }

  /**
     For current (2022-08-22) purposes, automatically call
     sqlite3ApiBootstrap().  That decision will be revisited at some
     point, as we really want client code to be able to call this to
     configure certain parts. Clients may modify
     globalThis.sqlite3ApiBootstrap.defaultConfig to tweak the default
     configuration used by a no-args call to sqlite3ApiBootstrap(),
     but must have first loaded their WASM module in order to be
     able to provide the necessary configuration state.
  */
  //console.warn("globalThis.sqlite3ApiConfig = ",globalThis.sqlite3ApiConfig);
  try{
    Module.sqlite3 = globalThis.sqlite3ApiBootstrap(SABC)
      /* Our customized sqlite3InitModule() in extern-post-js.js needs
         this to be able to pass the sqlite3 object off to the
         client. */;
  }catch(e){
    console.error("sqlite3ApiBootstrap() error:",e);
    throw e;
  }finally{
    delete globalThis.sqlite3ApiBootstrap;
    delete globalThis.sqlite3ApiConfig;
  }

}else{
  console.warn("This is not running in an Emscripten module context, so",
               "globalThis.sqlite3ApiBootstrap() is _not_ being called due to lack",
               "of config info for the WASM environment.",
               "It must be called manually.");
}
