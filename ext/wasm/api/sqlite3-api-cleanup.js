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

  In Emscripten builds it's run in the context of what amounts to a
  Module.postRun handler, though it's no longer actually a postRun
  handler because Emscripten 4.0 changed postRun semantics in an
  incompatible way.

  In terms of amalgamation code placement, this file is appended
  immediately after the final sqlite3-api-*.js piece. Those files
  cooperate to prepare sqlite3ApiBootstrap() and this file calls it.
  It is run within a context which gives it access to Emscripten's
  Module object, after sqlite3.wasm is loaded but before
  sqlite3ApiBootstrap() has been called.
*/
'use strict';
if( 'undefined' !== typeof Module ){ // presumably an Emscripten build
  try{
    const SABC = Object.assign(
      /**
         The WASM-environment-dependent configuration for
         sqlite3ApiBootstrap().
      */
      Object.create(null),
      globalThis.sqlite3ApiConfig || {}, {
        memory: ('undefined'!==typeof wasmMemory)
          ? wasmMemory
          : Module['wasmMemory'],
        exports: ('undefined'!==typeof wasmExports)
          ? wasmExports /* emscripten >=3.1.44 */
          : (Object.prototype.hasOwnProperty.call(Module,'wasmExports')
             ? Module['wasmExports']
             : Module['asm']/* emscripten <=3.1.43 */)
      },
    );

    /** Figure out if this is a 32- or 64-bit WASM build. */
    SABC.wasmPtrIR = 'number'===(typeof SABC.exports.sqlite3_libversion())
      ?  'i32' :'i64';

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
    Module.sqlite3 = globalThis.sqlite3ApiBootstrap(SABC)
    /* Our customized sqlite3InitModule() in extern-post-js.js needs
       this to be able to pass the sqlite3 object off to the
       client. */;
    delete globalThis.sqlite3ApiBootstrap;
    delete globalThis.sqlite3ApiConfig;
  }catch(e){
    console.error("sqlite3ApiBootstrap() error:",e);
    throw e;
  }
}else{
  console.warn("This is not running in an Emscripten module context, so",
               "globalThis.sqlite3ApiBootstrap() is _not_ being called due to lack",
               "of config info for the WASM environment.",
               "It must be called manually.");
}
