/*
  2022-05-22

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  This file is intended to be appended to the emcc-generated
  sqlite3.js via emcc:

  emcc ... -sMODULARIZE -sEXPORT_NAME=initSqlite3Module --post-js=THIS_FILE

  It is loaded by importing the emcc-generated sqlite3.js, then:

  initSqlite3Module({module object}).then(
    function(theModule){
      theModule.sqlite3 == an object containing this file's
      deliverables:
      {
        api: bindings for much of the core sqlite3 APIs,
        SQLite3: high-level OO API wrapper
      }
   });

  It is up to the caller to provide a module object compatible with
  emcc, but it can be a plain empty object. The object passed to
  initSqlite3Module() will get populated by the emscripten-generated
  bits and, in part, by the code from this file. Specifically, this file
  installs the `theModule.sqlite3` part shown above.

  The resulting sqlite3.api object wraps the standard sqlite3 C API in
  a way as close to its native form as JS allows for. The
  sqlite3.SQLite3 object provides a higher-level wrapper more
  appropriate for general client-side use in JS.

  Because using certain parts of the low-level API properly requires
  some degree of WASM-related magic, it is not recommended that that
  API be used as-is in client-level code. Rather, client code should
  use the higher-level OO API or write a custom wrapper on top of the
  lower-level API. In short, most of the C-style API is used in an
  intuitive manner from JS but any C-style APIs which take
  pointers-to-pointer arguments require WASM-specific interfaces
  installed by emcscripten-generated code. Those which take or return
  only integers, doubles, strings, or "plain" pointers to db or
  statement objects can be used in "as normal," noting that "pointers"
  in wasm are simply 32-bit integers.

  # Goals and Non-goals of this API

  Goals:

  - Except where noted in the non-goals, provide a more-or-less
    complete wrapper to the sqlite3 C API, insofar as WASM feature
    parity with C allows for. In fact, provide at least 3...

  - (1) The aforementioned C-style API. (2) An OO-style API on
    top of that, designed to run in the same thread (main window or
    Web Worker) as the C API. (3) A less-capable wrapper which can
    work across the main window/worker boundary, where the sqlite3 API
    is one of those and this wrapper is in the other. That
    constellation places some considerable limitations on how the API
    can be interacted with, but keeping the DB operations out of the
    UI thread is generally desirable.

  - Insofar as possible, support client-side storage using JS
    filesystem APIs. As of this writing, such things are still very
    much TODO.

  Non-goals:

  - As WASM is a web-centric technology and UTF-8 is the King of
    Encodings in that realm, there are no current plans to support the
    UTF16-related APIs. They would add a complication to the bindings
    for no appreciable benefit.

  - Supporting old or niche-market platforms. WASM is built for a
    modern web and requires modern platforms.

*/
if(!Module.postRun) Module.postRun = [];
/* ^^^^ the name Module is, in this setup, scope-local in the generated
   file sqlite3.js, with which this file gets combined at build-time. */
Module.postRun.push(function(namespace){
    'use strict';
    /* For reference: sql.js does essentially everything we want and
       it solves much of the wasm-related voodoo, but we'll need a
       different structure because we want the db connection to run in
       a worker thread and feed data back into the main
       thread. Regardless of those differences, it makes a great point
       of reference:

       https://github.com/sql-js/sql.js

       Some of the specific design goals here:

       - Bind a low-level sqlite3 API which is close to the native one
         in terms of usage.

       - Create a higher-level one, more akin to sql.js and
         node.js-style implementations. This one would speak directly
         to the low-level API. This API could be used by clients who
         import the low-level API directly into their main thread
         (which we don't want to recommend but also don't want to
         outright forbid).

       - Create a second higher-level one which speaks to the
         low-level API via worker messages. This one would be intended
         for use in the main thread, talking to the low-level UI via
         worker messages. Because workers have only a single message
         channel, some acrobatics will be needed here to feed async
         work results back into client-side callbacks (as those
         callbacks cannot simply be passed to the worker). Exactly
         what those acrobatics should look like is not yet entirely
         clear and much experimentation is pending.
    */

    const SQM = namespace/*the sqlite module object */;

    /** 
      Set up the main sqlite3 binding API here, mimicking the C API as
      closely as we can.

      Attribution: though not a direct copy/paste, much of what
      follows is strongly influenced by the sql.js implementation.
    */
    const api = {
        /* It is important that the following integer values match
           those from the C code. Ideally we could fetch them from the
           C API, e.g., in the form of a JSON object, but getting that
           JSON string constructed within our current confines is
           currently not worth the effort.

           Reminder to self: we could probably do so by adding the
           proverbial level of indirection, calling in to C to get it,
           and having that C func call an
           emscripten-installed/JS-implemented library function which
           builds the result object:

           const obj = {};
           sqlite3__get_enum(function(key,val){
               obj[key] = val;
           });

           but whether or not we can pass a function that way, via a
           (void*) is as yet unknown.
        */
        /* Minimum subset of sqlite result codes we'll need. */
        SQLITE_OK: 0,
        SQLITE_ROW: 100,
        SQLITE_DONE: 101,
        /* sqlite data types */
        SQLITE_INTEGER: 1,
        SQLITE_FLOAT: 2,
        SQLITE_TEXT: 3,
        SQLITE_BLOB: 4,
        SQLITE_NULL: 5,
        /* create_function() flags */
        SQLITE_DETERMINISTIC: 0x000000800,
        SQLITE_DIRECTONLY: 0x000080000,
        SQLITE_INNOCUOUS: 0x000200000,
        /* sqlite encodings, used for creating UDFs, noting that we
           will only support UTF8. */
        SQLITE_UTF8: 1
    };
    const cwrap = SQM.cwrap;
    [/* C-side functions to bind. Each entry is an array with 3 or 4
        elements:
        
        ["c-side name",
         "result type" (cwrap() syntax),
         [arg types in cwrap() syntax]
        ]

        If it has 4 elements, the first one is an alternate name to
        use for the JS-side binding. That's required when overloading
        a binding for two different uses.
     */
        ["sqlite3_bind_blob","number",["number", "number", "number", "number", "number"]],
        ["sqlite3_bind_double","number",["number", "number", "number"]],
        ["sqlite3_bind_int","number",["number", "number", "number"]],
        /*Noting that JS/wasm combo does not currently support 64-bit integers:
          ["sqlite3_bind_int64","number",["number", "number", "number"]],*/
        ["sqlite3_bind_null","void",["number"]],
        ["sqlite3_bind_parameter_count", "number", ["number"]],
        ["sqlite3_bind_parameter_index","number",["number", "string"]],
        ["sqlite3_bind_text","number",["number", "number", "number", "number", "number"]],
        ["sqlite3_changes", "number", ["number"]],
        ["sqlite3_clear_bindings","number",["number"]],
        ["sqlite3_close_v2", "number", ["number"]],
        ["sqlite3_column_blob","number", ["number", "number"]],
        ["sqlite3_column_bytes","number",["number", "number"]],
        ["sqlite3_column_count", "number", ["number"]],
        ["sqlite3_column_count","number",["number"]],
        ["sqlite3_column_double","number",["number", "number"]],
        ["sqlite3_column_int","number",["number", "number"]],
        /*Noting that JS/wasm combo does not currently support 64-bit integers:
          ["sqlite3_column_int64","number",["number", "number"]],*/
        ["sqlite3_column_name","string",["number", "number"]],
        ["sqlite3_column_text","string",["number", "number"]],
        ["sqlite3_column_type","number",["number", "number"]],
        ["sqlite3_compileoption_get", "string", ["number"]],
        ["sqlite3_compileoption_used", "number", ["string"]],
        ["sqlite3_create_function_v2", "number",
         ["number", "string", "number", "number","number",
          "number", "number", "number", "number"]],
        ["sqlite3_data_count", "number", ["number"]],
        ["sqlite3_db_filename", "string", ["number", "string"]],
        ["sqlite3_errmsg", "string", ["number"]],
        ["sqlite3_exec", "number", ["number", "string", "number", "number", "number"]],
        ["sqlite3_finalize", "number", ["number"]],
        ["sqlite3_interrupt", "void", ["number"]],
        ["sqlite3_libversion", "string", []],
        ["sqlite3_open", "number", ["string", "number"]],
        //["sqlite3_open_v2", "number", ["string", "number", "number", "string"]],
        //^^^^ TODO: add the flags needed for the 3rd arg
        ["sqlite3_prepare_v2", "number", ["number", "string", "number", "number", "number"]],
        ["sqlite3_prepare_v2_sqlptr", "sqlite3_prepare_v2",
         /* Impl which requires that the 2nd argument be a pointer to
            the SQL, instead of a string. This is used for cases where
            we require a non-NULL value for the final argument. We may
            or may not need this, depending on how our higher-level
            API shapes up, but this code's spiritual guide (sql.js)
            uses it we we'll include it. */
         "number", ["number", "number", "number", "number", "number"]],
        ["sqlite3_reset", "number", ["number"]],
        ["sqlite3_result_blob",null,["number", "number", "number", "number"]],
        ["sqlite3_result_double",null,["number", "number"]],
        ["sqlite3_result_error",null,["number", "string", "number"]],
        ["sqlite3_result_int",null,["number", "number"]],
        ["sqlite3_result_null",null,["number"]],
        ["sqlite3_result_text",null,["number", "string", "number", "number"]],
        ["sqlite3_sourceid", "string", []],
        ["sqlite3_sql", "string", ["number"]],
        ["sqlite3_step", "number", ["number"]],
        ["sqlite3_value_blob", "number", ["number"]],
        ["sqlite3_value_bytes","number",["number"]],
        ["sqlite3_value_double","number",["number"]],
        ["sqlite3_value_text", "string", ["number"]],
        ["sqlite3_value_type", "number", ["number"]]
        //["sqlite3_normalized_sql", "string", ["number"]]
    ].forEach(function(a){
        const k = (4==a.length) ? a.shift() : a[0];
        api[k] = cwrap.apply(this, a);
    });

    /* What follows is colloquially known as "OO API #1". It is a
       binding of the sqlite3 API which is designed to be run within
       the same thread (main or worker) as the one in which the
       sqlite3 WASM binding was initialized. This wrapper cannot use
       the sqlite3 binding if, e.g., the wrapper is in the main thread
       and the sqlite3 API is in a worker. */

    /** Memory for use in some pointer-to-pointer-passing routines. */
    const pPtrArg = stackAlloc(4);
    /** Throws a new error, concatenating all args with a space between
        each. */
    const toss = function(){
        throw new Error(Array.prototype.join.call(arguments, ' '));
    };

    /**
       The DB class wraps a sqlite3 db handle.

       It accepts the following argument signatures:

       - ()
       - (undefined) (same effect as ())
       - (Uint8Array holding an sqlite3 db image)

       It always generates a random filename and sets is to
       the `filename` property of this object.

       Developer's note: the reason it does not (any longer) support
       ":memory:" as a name is because we can apparently only export
       images of DBs which are stored in the pseudo-filesystem
       provided by the JS APIs. Since exporting and importing images
       is an important usability feature for this class, ":memory:"
       DBs are not supported (until/unless we can find a way to export
       those as well). The naming semantics will certainly evolve as
       this API does.
    */
    const DB = function(arg){
        const fn = "db-"+((Math.random() * 10000000) | 0)+
              "-"+((Math.random() * 10000000) | 0)+".sqlite3";
        let buffer;
        if(name instanceof Uint8Array){
            buffer = arg;
            arg = undefined;
        }else if(arguments.length && undefined!==arg){
            toss("Invalid arguments to DB constructor.",
                 "Expecting no args, undefined, or a",
                 "sqlite3 file as a Uint8Array.");
        }
        if(buffer){
            FS.createDataFile("/", fn, buffer, true, true);
        }
        setValue(pPtrArg, 0, "i32");
        this.checkRc(api.sqlite3_open(fn, pPtrArg));
        this._pDb = getValue(pPtrArg, "i32");
        this.filename = fn;
        this._statements = {/*map of open Stmt _pointers_ to Stmt*/};
        this._udfs = {/*map of UDF names to wasm function _pointers_*/};
    };

    /**
       Internal-use enum for mapping JS types to DB-bindable types.
       These do not (and need not) line up with the SQLITE_type
       values. All values in this enum must be truthy and distinct
       but they need not be numbers.
    */
    const BindTypes = {
        null: 1,
        number: 2,
        string: 3,
        boolean: 4,
        blob: 5
    };
    BindTypes['undefined'] == BindTypes.null;

    /**
       This class wraps sqlite3_stmt. Calling this constructor
       directly will trigger an exception. Use DB.prepare() to create
       new instances.
    */
    const Stmt = function(){
        if(BindTypes!==arguments[2]){
            toss("Do not call the Stmt constructor directly. Use DB.prepare().");
        }
        this.db = arguments[0];
        this._pStmt = arguments[1];
        this.columnCount = api.sqlite3_column_count(this._pStmt);
        this.parameterCount = api.sqlite3_bind_parameter_count(this._pStmt);
        this._allocs = [/*list of alloc'd memory blocks for bind() values*/]
    };

    /** Throws if the given DB has been closed, else it is returned. */
    const affirmDbOpen = function(db){
        if(!db._pDb) toss("DB has been closed.");
        return db;
    };

    /** Returns true if n is a 32-bit (signed) integer,
        else false. */
    const isInt32 = function(n){
        return (n===n|0 && n<0xFFFFFFFF) ? true : undefined;
    };

    /**
       Expects to be passed (arguments) from DB.exec() and
       DB.execMulti(). Does the argument processing/validation, throws
       on error, and returns a new object on success:

       { sql: the SQL, obt: optionsObj, cbArg: function}

       cbArg is only set if the opt.callback is set, in which case
       it's a function which expects to be passed the current Stmt
       and returns the callback argument of the type indicated by
       the input arguments.
    */
    const parseExecArgs = function(args){
        const out = {};
        switch(args.length){
            case 1:
                if('string'===typeof args[0]){
                    out.sql = args[0];
                    out.opt = {};
                }else if(args[0] && 'object'===typeof args[0]){
                    out.opt = args[0];
                    out.sql = out.opt.sql;
                }
            break;
            case 2:
                out.sql = args[0];
                out.opt = args[1];
            break;
            default: toss("Invalid argument count for exec().");
        };
        if('string'!==typeof out.sql) toss("Missing SQL argument.");
        if(out.opt.callback){
            switch((undefined===out.opt.rowMode)
                    ? 'stmt' : out.opt.rowMode) {
                case 'object': out.cbArg = (stmt)=>stmt.get({}); break;
                case 'array': out.cbArg = (stmt)=>stmt.get([]); break;
                case 'stmt': out.cbArg = (stmt)=>stmt; break;
                default: toss("Invalid rowMode:",out.opt.rowMode);
            }
        }
        return out;
    };

    /** If object opts has _its own_ property named p then that
        property's value is returned, else dflt is returned. */
    const getOwnOption = (opts, p, dflt)=>
        opts.hasOwnProperty(p) ? opts[p] : dflt;

    DB.prototype = {
        /**
           Expects to be given an sqlite3 API result code. If it is
           falsy, this function returns this object, else it throws an
           exception with an error message from sqlite3_errmsg(),
           using this object's db handle. Note that if it's passed a
           non-error code like SQLITE_ROW or SQLITE_DONE, it will
           still throw but the error string might be "Not an error."
           The various non-0 non-error codes need to be checked for in
           client code where they are expected.
        */
        checkRc: function(sqliteResultCode){
            if(!sqliteResultCode) return this;
            toss("sqlite result code",sqliteResultCode+":",
                 api.sqlite3_errmsg(this._pDb) || "Unknown db error.");
        },
        /**
           Finalizes all open statements and closes this database
           connection. This is a no-op if the db has already been
           closed.
        */
        close: function(){
            if(this._pDb){
                let s;
                const that = this;
                Object.keys(this._statements).forEach(function(k,s){
                    delete that._statements[k];
                    if(s && s._pStmt) s.finalize();
                });
                Object.values(this._udfs).forEach(SQM.removeFunction);
                delete this._udfs;
                delete this._statements;
                delete this.filename;
                api.sqlite3_close_v2(this._pDb);
                delete this._pDb;
            }
        },
        /**
           Similar to this.filename but will return NULL for
           special names like ":memory:". Not of much use until
           we have filesystem support. Throws if the DB has
           been closed. If passed an argument it then it will return
           the filename of the ATTACHEd db with that name, else it assumes
           a name of `main`.
        */
        fileName: function(dbName){
            return api.sqlite3_db_filename(affirmDbOpen(this)._pDb, dbName||"main");
        },
        /**
           Compiles the given SQL and returns a prepared Stmt. This is
           the only way to create new Stmt objects. Throws on error.
        */
        prepare: function(sql){
            affirmDbOpen(this);
            setValue(pPtrArg,0,"i32");
            this.checkRc(api.sqlite3_prepare_v2(this._pDb, sql, -1, pPtrArg, null));
            const pStmt = getValue(pPtrArg, "i32");
            if(!pStmt) toss("Empty SQL is not permitted.");
            const stmt = new Stmt(this, pStmt, BindTypes);
            this._statements[pStmt] = stmt;
            return stmt;
        },
        /**
           This function works like execMulti(), and takes the same
           arguments, but is more efficient (performs much less work)
           when the input SQL is only a single statement. If passed a
           multi-statement SQL, it only processes the first one.

           This function supports one additional option not used by
           execMulti():

           - .multi: if true, this function acts as a proxy for
             execMulti().
        */
        exec: function(/*(sql [,optionsObj]) or (optionsObj)*/){
            affirmDbOpen(this);
            const arg = parseExecArgs(arguments);
            if(!arg.sql) return this;
            else if(arg.opt.multi){
                return this.execMulti(arg, undefined, BindTypes);
            }
            const opt = arg.opt;
            let stmt;
            try {
                stmt = this.prepare(arg.sql);
                if(opt.bind) stmt.bind(opt.bind);
                if(opt.callback){
                    while(stmt.step()){
                        stmt._isLocked = true;
                        opt.callback(arg.cbArg(stmt), stmt);
                        stmt._isLocked = false;
                    }
                }else{
                    stmt.step();
                }
            }finally{
                if(stmt){
                    delete stmt._isLocked;
                    stmt.finalize();
                }
            }
            return this;

        }/*exec()*/,
        /**
           Executes one or more SQL statements. Its arguments
           must be either (sql,optionsObject) or (optionsObject).
           In the latter case, optionsObject.sql must contain the
           SQL to execute. Returns this object. Throws on error.

           If no SQL is provided, or a non-string is provided, an
           exception is triggered. Empty SQL, on the other hand, is
           simply a no-op.

           The optional options object may contain any of the following
           properties:

           - .sql = the SQL to run (unless it's provided as the first
             argument).

           - .bind = a single value valid as an argument for
             Stmt.bind(). This is ONLY applied to the FIRST non-empty
             statement in the SQL which has any bindable
             parameters. (Empty statements are skipped entirely.)

           - .callback = a function which gets called for each row of
             the FIRST statement in the SQL (if it has any result
             rows). The second argument passed to the callback is
             always the current Stmt object (so that the caller
             may collect column names, or similar). The first
             argument passed to the callback defaults to the current
             Stmt object but may be changed with ...

           - .rowMode = a string describing what type of argument
             should be passed as the first argument to the callback. A
             value of 'object' causes the results of `stmt.get({})` to
             be passed to the object. A value of 'array' causes the
             results of `stmt.get([])` to be passed to the callback.
             A value of 'stmt' is equivalent to the default, passing
             the current Stmt to the callback (noting that it's always
             passed as the 2nd argument). Any other value triggers an
             exception.

           - saveSql = an optional array. If set, the SQL of each
             executed statement is appended to this array before the
             statement is executed (but after it is prepared - we
             don't have the string until after that). Empty SQL
             statements are elided.

           ACHTUNG #1: The callback MUST NOT modify the Stmt
           object. Calling any of the Stmt.get() variants,
           Stmt.getColumnName(), or simililar, is legal, but calling
           step() or finalize() is not. Routines which are illegal
           in this context will trigger an exception.

           ACHTUNG #2: The semantics of the `bind` and `callback`
           options may well change or those options may be removed
           altogether for this function (but retained for exec()).
        */
        execMulti: function(/*(sql [,obj]) || (obj)*/){
            affirmDbOpen(this);
            const arg = (BindTypes===arguments[2]
                         /* ^^^ Being passed on from exec() */
                         ? arguments[0] : parseExecArgs(arguments));
            if(!arg.sql) return this;
            const opt = arg.opt;
            const stack = stackSave();
            let stmt;
            let bind = opt.bind;
            let rowMode = (
                (opt.callback && opt.rowMode)
                    ? opt.rowMode : false);
            try{
                let pSql = SQM.allocateUTF8OnStack(arg.sql)
                const pzTail = stackAlloc(4);
                while(getValue(pSql, "i8")){
                    setValue(pPtrArg, 0, "i32");
                    setValue(pzTail, 0, "i32");
                    this.checkRc(api.sqlite3_prepare_v2_sqlptr(
                        this._pDb, pSql, -1, pPtrArg, pzTail
                    ));
                    const pStmt = getValue(pPtrArg, "i32");
                    pSql = getValue(pzTail, "i32");
                    if(!pStmt) continue;
                    if(opt.saveSql){
                        opt.saveSql.push(api.sqlite3_sql(pStmt).trim());
                    }
                    stmt = new Stmt(this, pStmt, BindTypes);
                    if(bind && stmt.parameterCount){
                        stmt.bind(bind);
                        bind = null;
                    }
                    if(opt.callback && null!==rowMode){
                        while(stmt.step()){
                            stmt._isLocked = true;
                            callback(arg.cbArg(stmt), stmt);
                            stmt._isLocked = false;
                        }
                        rowMode = null;
                    }else{
                        // Do we need to while(stmt.step()){} here?
                        stmt.step();
                    }
                    stmt.finalize();
                    stmt = null;
                }
            }finally{
                if(stmt){
                    delete stmt._isLocked;
                    stmt.finalize();
                }
                stackRestore(stack);
            }
            return this;
        }/*execMulti()*/,
        /**
           Creates a new scalar UDF (User-Defined Function) which is
           accessible via SQL code. This function may be called in any
           of the following forms:

           - (name, function)
           - (name, function, optionsObject)
           - (name, optionsObject)
           - (optionsObject)

           In the final two cases, the function must be defined as the
           'callback' property of the options object. In the final
           case, the function's name must be the 'name' property.

           This can only be used to create scalar functions, not
           aggregate or window functions. UDFs cannot be removed from
           a DB handle after they're added.

           On success, returns this object. Throws on error.

           When called from SQL, arguments to the UDF, and its result,
           will be converted between JS and SQL with as much fidelity
           as is feasible, triggering an exception if a type
           conversion cannot be determined. Some freedom is afforded
           to numeric conversions due to friction between the JS and C
           worlds: integers which are larger than 32 bits will be
           treated as doubles, as JS does not support 64-bit integers
           and it is (as of this writing) illegal to use WASM
           functions which take or return 64-bit integers from JS.

           The optional options object may contain flags to modify how
           the function is defined:

           - .arity: the number of arguments which SQL calls to this
             function expect or require. The default value is the
             callback's length property (i.e. the number of declared
             parameters it has). A value of -1 means that the function
             is variadic and may accept any number of arguments, up to
             sqlite3's compile-time limits. sqlite3 will enforce the
             argument count if is zero or greater.

           The following properties correspond to flags documented at:

           https://sqlite.org/c3ref/create_function.html

           - .deterministic = SQLITE_DETERMINISTIC
           - .directOnly = SQLITE_DIRECTONLY
           - .innocuous = SQLITE_INNOCUOUS


           Maintenance reminder: the ability to add new
           WASM-accessible functions to the runtime requires that the
           WASM build is compiled with emcc's `-sALLOW_TABLE_GROWTH`
           flag.
        */
        createFunction: function f(name, callback,opt){
            switch(arguments.length){
                case 1: /* (optionsObject) */
                    opt = name;
                    name = opt.name;
                    callback = opt.callback;
                    break;
                case 2: /* (name, callback|optionsObject) */
                    if(!(callback instanceof Function)){
                        opt = callback;
                        callback = opt.callback;
                    }
                    break;
                default: break;
            }
            if(!opt) opt = {};
            if(!(callback instanceof Function)){
                toss("Invalid arguments: expecting a callback function.");
            }else if('string' !== typeof name){
                toss("Invalid arguments: missing function name.");
            }
            if(!f._extractArgs){
                /* Static init */
                f._extractArgs = function(argc, pArgv){
                    let i, pVal, valType, arg;
                    const tgt = [];
                    for(i = 0; i < argc; ++i){
                        pVal = getValue(pArgv + (4 * i), "i32");
                        valType = api.sqlite3_value_type(pVal);
                        switch(valType){
                            case api.SQLITE_INTEGER:
                            case api.SQLITE_FLOAT:
                                arg = api.sqlite3_value_double(pVal);
                                break;
                            case SQLITE_TEXT:
                                arg = api.sqlite3_value_text(pVal);
                                break;
                            case SQLITE_BLOB:{
                                const n = api.sqlite3_value_bytes(ptr);
                                const pBlob = api.sqlite3_value_blob(ptr);
                                arg = new Uint8Array(n);
                                let i;
                                for(i = 0; i < n; ++i) arg[i] = HEAP8[pBlob+i];
                                break;
                            }
                            default:
                                arg = null; break;
                        }
                        tgt.push(arg);
                    }
                    return tgt;
                }/*_extractArgs()*/;
                f._setResult = function(pCx, val){
                    switch(typeof val) {
                        case 'boolean':
                            api.sqlite3_result_int(pCx, val ? 1 : 0);
                            break;
                        case 'number': {
                            (isInt32(val)
                             ? api.sqlite3_result_int
                             : api.sqlite3_result_double)(pCx, val);
                            break;
                        }
                        case 'string':
                            api.sqlite3_result_text(pCx, val, -1,
                                                  -1/*==SQLITE_TRANSIENT*/);
                            break;
                        case 'object':
                            if(null===val) {
                                api.sqlite3_result_null(pCx);
                                break;
                            }else if(undefined!==val.length){
                                const pBlob =
                                      SQM.allocate(val, SQM.ALLOC_NORMAL);
                                api.sqlite3_result_blob(pCx, pBlob, val.length, -1/*==SQLITE_TRANSIENT*/);
                                SQM._free(blobptr);
                                break;
                            }
                            // else fall through
                        default:
                            toss("Don't not how to handle this UDF result value:",val);
                    };
                }/*_setResult()*/;
            }/*static init*/
            const wrapper = function(pCx, argc, pArgv){
                try{
                    f._setResult(pCx, callback.apply(null, f._extractArgs(argc, pArgv)));
                }catch(e){
                    api.sqlite3_result_error(pCx, e.message, -1);
                }
            };
            const pUdf = SQM.addFunction(wrapper, "viii");
            let fFlags = 0;
            if(getOwnOption(opt, 'deterministic')) fFlags |= api.SQLITE_DETERMINISTIC;
            if(getOwnOption(opt, 'directOnly')) fFlags |= api.SQLITE_DIRECTONLY;
            if(getOwnOption(opt, 'innocuous')) fFlags |= api.SQLITE_INNOCUOUS;
            name = name.toLowerCase();
            try {
                this.checkRc(api.sqlite3_create_function_v2(
                    this._pDb, name,
                    (opt.hasOwnProperty('arity') ? +opt.arity : callback.length),
                    api.SQLITE_UTF8 | fFlags, null/*pApp*/, pUdf,
                    null/*xStep*/, null/*xFinal*/, null/*xDestroy*/));
            }catch(e){
                SQM.removeFunction(pUdf);
                throw e;
            }
            if(this._udfs.hasOwnProperty(name)){
                SQM.removeFunction(this._udfs[name]);
            }
            this._udfs[name] = pUdf;
            return this;
        }/*createFunction()*/,
        /**
           Prepares the given SQL, step()s it one time, and returns
           the value of the first result column. If it has no results,
           undefined is returned. If passed a second argument, it is
           treated like an argument to Stmt.bind(), so may be any type
           supported by that function. Throws on error (e.g. malformed
           SQL).
        */
        selectValue: function(sql,bind){
            let stmt, rc;
            try {
                stmt = this.prepare(sql).bind(bind);
                if(stmt.step()) rc = stmt.get(0);
            }finally{
                if(stmt) stmt.finalize();
            }
            return rc;
        },

        /**
           Exports a copy of this db's file as a Uint8Array and
           returns it. It is technically not legal to call this while
           any prepared statement are currently active. Throws if this
           db is not open.

           Maintenance reminder: the corresponding sql.js impl of this
           feature closes the current db, finalizing any active
           statements and (seemingly unnecessarily) destroys any UDFs,
           copies the file, and then re-opens it (without restoring
           the UDFs). Those gymnastics are not necessary on the tested
           platform but might be necessary on others. Because of that
           eventuality, this interface currently enforces that no
           statements are active when this is run. It will throw if
           any are.
        */
        exportBinaryImage: function(){
            affirmDbOpen(this);
            if(Object.keys(this._statements).length){
                toss("Cannot export with prepared statements active!",
                     "finalize() all statements and try again.");
            }
            const img = FS.readFile(this.filename, {encoding:"binary"});
            return img;
        }
    }/*DB.prototype*/;


    /** Throws if the given Stmt has been finalized, else stmt is
        returned. */
    const affirmStmtOpen = function(stmt){
        if(!stmt._pStmt) toss("Stmt has been closed.");
        return stmt;
    };

    /** Returns an opaque truthy value from the BindTypes
        enum if v's type is a valid bindable type, else
        returns a falsy value. As a special case, a value of
        undefined is treated as a bind type of null. */
    const isSupportedBindType = function(v){
        let t = BindTypes[(null===v||undefined===v) ? 'null' : typeof v];
        switch(t){
            case BindTypes.boolean:
            case BindTypes.null:
            case BindTypes.number:
            case BindTypes.string:
                return t;
            default:
                if(v instanceof Uint8Array) return BindTypes.blob;
                return undefined;
        }
    };

    /**
       If isSupportedBindType(v) returns a truthy value, this
       function returns that value, else it throws.
    */
    const affirmSupportedBindType = function(v){
        return isSupportedBindType(v) || toss("Unsupport bind() argument type.");
    };

    /**
       If key is a number and within range of stmt's bound parameter
       count, key is returned.

       If key is not a number then it is checked against named
       parameters. If a match is found, its index is returned.

       Else it throws.
    */
    const affirmParamIndex = function(stmt,key){
        const n = ('number'===typeof key)
              ? key : api.sqlite3_bind_parameter_index(stmt._pStmt, key);
        if(0===n || (n===key && (n!==(n|0)/*floating point*/))){
            toss("Invalid bind() parameter name: "+key);
        }
        else if(n<1 || n>stmt.parameterCount) toss("Bind index",key,"is out of range.");
        return n;
    };

    /** Throws if ndx is not an integer or if it is out of range
        for stmt.columnCount, else returns stmt.

        Reminder: this will also fail after the statement is finalized
        but the resulting error will be about an out-of-bounds column
        index.
    */
    const affirmColIndex = function(stmt,ndx){
        if((ndx !== (ndx|0)) || ndx<0 || ndx>=stmt.columnCount){
            toss("Column index",ndx,"is out of range.");
        }
        return stmt;
    };

    /**
       If stmt._isLocked is truthy, this throws an exception
       complaining that the 2nd argument (an operation name,
       e.g. "bind()") is not legal while the statement is "locked".
       Locking happens before an exec()-like callback is passed a
       statement, to ensure that the callback does not mutate or
       finalize the statement. If it does not throw, it returns stmt.
    */
    const affirmUnlocked = function(stmt,currentOpName){
        if(stmt._isLocked){
            toss("Operation is illegal when statement is locked:",currentOpName);
        }
        return stmt;
    };

    /**
       Binds a single bound parameter value on the given stmt at the
       given index (numeric or named) using the given bindType (see
       the BindTypes enum) and value. Throws on error. Returns stmt on
       success.
    */
    const bindOne = function f(stmt,ndx,bindType,val){
        affirmUnlocked(stmt, 'bind()');
        if(!f._){
            f._ = {
                string: function(stmt, ndx, val, asBlob){
                    const bytes = intArrayFromString(val,true);
                    const pStr = SQM.allocate(bytes, ALLOC_NORMAL);
                    stmt._allocs.push(pStr);
                    const func =  asBlob ? api.sqlite3_bind_blob : api.sqlite3_bind_text;
                    return func(stmt._pStmt, ndx, pStr, bytes.length, 0);
                }
            };
        }
        affirmSupportedBindType(val);
        ndx = affirmParamIndex(stmt,ndx);
        let rc = 0;
        switch((null===val || undefined===val) ? BindTypes.null : bindType){
            case BindTypes.null:
                rc = api.sqlite3_bind_null(stmt._pStmt, ndx);
                break;
            case BindTypes.string:{
                rc = f._.string(stmt, ndx, val, false);
                break;
            }
            case BindTypes.number: {
                const m = (isInt32(val)
                           ? api.sqlite3_bind_int
                           /*It's illegal to bind a 64-bit int
                             from here*/
                           : api.sqlite3_bind_double);
                rc = m(stmt._pStmt, ndx, val);
                break;
            }
            case BindTypes.boolean:
                rc = api.sqlite3_bind_int(stmt._pStmt, ndx, val ? 1 : 0);
                break;
            case BindTypes.blob: {
                if('string'===typeof val){
                    rc = f._.string(stmt, ndx, val, true);
                }else{
                    const len = val.length;
                    if(undefined===len){
                        toss("Binding a value as a blob requires",
                             "that it have a length member.");
                    }
                    const pBlob = SQM.allocate(val, ALLOC_NORMAL);
                    stmt._allocs.push(pBlob);
                    rc = api.sqlite3_bind_blob(stmt._pStmt, ndx, pBlob, len, 0);
                }
            }
            default: toss("Unsupported bind() argument type.");
        }
        if(rc) stmt.db.checkRc(rc);
        return stmt;
    };

    /** Frees any memory explicitly allocated for the given
        Stmt object. Returns stmt. */
    const freeBindMemory = function(stmt){
        let m;
        while(undefined !== (m = stmt._allocs.pop())){
            SQM._free(m);
        }
        return stmt;
    };
    
    Stmt.prototype = {
        /**
           "Finalizes" this statement. This is a no-op if the
           statement has already been finalizes. Returns
           undefined. Most methods in this class will throw if called
           after this is.
        */
        finalize: function(){
            if(this._pStmt){
                affirmUnlocked(this,'finalize()');
                freeBindMemory(this);
                delete this.db._statements[this._pStmt];
                api.sqlite3_finalize(this._pStmt);
                delete this.columnCount;
                delete this.parameterCount;
                delete this._pStmt;
                delete this.db;
                delete this._isLocked;
            }
        },
        /** Clears all bound values. Returns this object.
            Throws if this statement has been finalized. */
        clearBindings: function(){
            freeBindMemory(
                affirmUnlocked(affirmStmtOpen(this), 'clearBindings()')
            );
            api.sqlite3_clear_bindings(this._pStmt);
            this._mayGet = false;
            return this;
        },
        /**
           Resets this statement so that it may be step()ed again
           from the beginning. Returns this object. Throws if this
           statement has been finalized.

           If passed a truthy argument then this.clearBindings() is
           also called, otherwise any existing bindings, along with
           any memory allocated for them, are retained.
        */
        reset: function(alsoClearBinds){
            affirmUnlocked(this,'reset()');
            if(alsoClearBinds) this.clearBindings();
            api.sqlite3_reset(affirmStmtOpen(this)._pStmt);
            this._mayGet = false;
            return this;
        },
        /**
           Binds one or more values to its bindable parameters. It
           accepts 1 or 2 arguments:

           If passed a single argument, it must be either an array, an
           object, or a value of a bindable type (see below).

           If passed 2 arguments, the first one is the 1-based bind
           index or bindable parameter name and the second one must be
           a value of a bindable type.

           Bindable value types:

           - null is bound as NULL.

           - undefined as a standalone value is a no-op intended to
             simplify certain client-side use cases: passing undefined
             as a value to this function will not actually bind
             anything and this function will skip confirmation that
             binding is even legal. (Those semantics simplify certain
             client-side uses.) Conversely, a value of undefined as an
             array or object property when binding an array/object
             (see below) is treated the same as null.

           - Numbers are bound as either doubles or integers: doubles
             if they are larger than 32 bits, else double or int32,
             depending on whether they have a fractional part. (It is,
             as of this writing, illegal to call (from JS) a WASM
             function which either takes or returns an int64.)
             Booleans are bound as integer 0 or 1. It is not expected
             the distinction of binding doubles which have no
             fractional parts is integers is significant for the
             majority of clients due to sqlite3's data typing
             model. This API does not currently support the BigInt
             type.

           - Strings are bound as strings (use bindAsBlob() to force
             blob binding).

           - Uint8Array instances are bound as blobs.

           If passed an array, each element of the array is bound at
           the parameter index equal to the array index plus 1
           (because arrays are 0-based but binding is 1-based).

           If passed an object, each object key is treated as a
           bindable parameter name. The object keys _must_ match any
           bindable parameter names, including any `$`, `@`, or `:`
           prefix. Because `$` is a legal identifier chararacter in
           JavaScript, that is the suggested prefix for bindable
           parameters.

           It returns this object on success and throws on
           error. Errors include:

           - Any bind index is out of range, a named bind parameter
             does not match, or this statement has no bindable
             parameters.

           - Any value to bind is of an unsupported type.

           - Passed no arguments or more than two.

           - The statement has been finalized.
        */
        bind: function(/*[ndx,] arg*/){
            affirmStmtOpen(this);
            let ndx, arg;
            switch(arguments.length){
                case 1: ndx = 1; arg = arguments[0]; break;
                case 2: ndx = arguments[0]; arg = arguments[1]; break;
                default: toss("Invalid bind() arguments.");
            }
            if(undefined===arg){
                /* It might seem intuitive to bind undefined as NULL
                   but this approach simplifies certain client-side
                   uses when passing on arguments between 2+ levels of
                   functions. */
                return this;
            }else if(!this.parameterCount){
                toss("This statement has no bindable parameters.");
            }
            this._mayGet = false;
            if(null===arg){
                /* bind NULL */
                return bindOne(this, ndx, BindTypes.null, arg);
            }
            else if(Array.isArray(arg)){
                /* bind each entry by index */
                if(1!==arguments.length){
                    toss("When binding an array, an index argument is not permitted.");
                }
                arg.forEach((v,i)=>bindOne(this, i+1, affirmSupportedBindType(v), v));
                return this;
            }
            else if('object'===typeof arg/*null was checked above*/){
                /* bind by name */
                if(1!==arguments.length){
                    toss("When binding an object, an index argument is not permitted.");
                }
                Object.keys(arg)
                    .forEach(k=>bindOne(this, k,
                                        affirmSupportedBindType(arg[k]),
                                        arg[k]));
                return this;
            }else{
                return bindOne(this, ndx,
                               affirmSupportedBindType(arg), arg);
            }
            toss("Should not reach this point.");
        },
        /**
           Special case of bind() which binds the given value
           using the BLOB binding mechanism instead of the default
           selected one for the value. The ndx may be a numbered
           or named bind index. The value must be of type string,
           Uint8Array, or null/undefined (both treated as null).

           If passed a single argument, a bind index of 1 is assumed.
        */
        bindAsBlob: function(ndx,arg){
            affirmStmtOpen(this);
            if(1===arguments.length){
                ndx = 1;
                arg = arguments[0];
            }
            const t = affirmSupportedBindType(arg);
            if(BindTypes.string !== t && BindTypes.blob !== t
               && BindTypes.null !== t){
                toss("Invalid value type for bindAsBlob()");
            }
            this._mayGet = false;
            return bindOne(this, ndx, BindTypes.blob, arg);
        },
        /**
           Steps the statement one time. If the result indicates that
           a row of data is available, true is returned.  If no row of
           data is available, false is returned.  Throws on error.
        */
        step: function(){
            affirmUnlocked(this, 'step()');
            const rc = api.sqlite3_step(affirmStmtOpen(this)._pStmt);
            switch(rc){
                case api.SQLITE_DONE: return this._mayGet = false;
                case api.SQLITE_ROW: return this._mayGet = true;
                default:
                    this._mayGet = false;
                    console.warn("sqlite3_step() rc=",rc,"SQL =",
                                 api.sqlite3_sql(this._pStmt));
                    this.db.checkRc(rc);
            };
        },
        /**
           Fetches the value from the given 0-based column index of
           the current data row, throwing if index is out of range. 

           Requires that step() has just returned a truthy value, else
           an exception is thrown.

           By default it will determine the data type of the result
           automatically. If passed a second arugment, it must be one
           of the enumeration values for sqlite3 types, which are
           defined as members of the sqlite3 module: SQLITE_INTEGER,
           SQLITE_FLOAT, SQLITE_TEXT, SQLITE_BLOB. Any other value,
           except for undefined, will trigger an exception. Passing
           undefined is the same as not passing a value. It is legal
           to, e.g., fetch an integer value as a string, in which case
           sqlite3 will convert the value to a string.

           If ndx is an array, this function behaves a differently: it
           assigns the indexes of the array, from 0 to the number of
           result columns, to the values of the corresponding column,
           and returns that array.

           If ndx is a plain object, this function behaves even
           differentlier: it assigns the properties of the object to
           the values of their corresponding result columns.

           Blobs are returned as Uint8Array instances.

           Potential TODO: add type ID SQLITE_JSON, which fetches the
           result as a string and passes it (if it's not null) to
           JSON.parse(), returning the result of that. Until then,
           getJSON() can be used for that.
        */
        get: function(ndx,asType){
            if(!affirmStmtOpen(this)._mayGet){
                toss("Stmt.step() has not (recently) returned true.");
            }
            if(Array.isArray(ndx)){
                let i = 0;
                while(i<this.columnCount){
                    ndx[i] = this.get(i++);
                }
                return ndx;
            }else if(ndx && 'object'===typeof ndx){
                let i = 0;
                while(i<this.columnCount){
                    ndx[api.sqlite3_column_name(this._pStmt,i)] = this.get(i++);
                }
                return ndx;
            }
            affirmColIndex(this, ndx);
            switch(undefined===asType
                   ? api.sqlite3_column_type(this._pStmt, ndx)
                   : asType){
                case api.SQLITE_NULL: return null;
                case api.SQLITE_INTEGER:{
                    return 0 | api.sqlite3_column_double(this._pStmt, ndx);
                    /* ^^^^^^^^ strips any fractional part and handles
                       handles >32bits */
                }
                case api.SQLITE_FLOAT:
                    return api.sqlite3_column_double(this._pStmt, ndx);
                case api.SQLITE_TEXT:
                    return api.sqlite3_column_text(this._pStmt, ndx);
                case api.SQLITE_BLOB: {
                    const n = api.sqlite3_column_bytes(this._pStmt, ndx);
                    const ptr = api.sqlite3_column_blob(this._pStmt, ndx);
                    const rc = new Uint8Array(n);
                    for(let i = 0; i < n; ++i) rc[i] = HEAP8[ptr + i];
                    return rc;
                }
                default: toss("Don't know how to translate",
                              "type of result column #"+ndx+".");
            }
            abort("Not reached.");
        },
        /** Equivalent to get(ndx) but coerces the result to an
            integer. */
        getInt: function(ndx){return this.get(ndx,api.SQLITE_INTEGER)},
        /** Equivalent to get(ndx) but coerces the result to a
            float. */
        getFloat: function(ndx){return this.get(ndx,api.SQLITE_FLOAT)},
        /** Equivalent to get(ndx) but coerces the result to a
            string. */
        getString: function(ndx){return this.get(ndx,api.SQLITE_TEXT)},
        /** Equivalent to get(ndx) but coerces the result to a
            Uint8Array. */
        getBlob: function(ndx){return this.get(ndx,api.SQLITE_BLOB)},
        /**
           A convenience wrapper around get() which fetches the value
           as a string and then, if it is not null, passes it to
           JSON.parse(), returning that result. Throws if parsing
           fails. If the result is null, null is returned. An empty
           string, on the other hand, will trigger an exception.
        */
        getJSON: function(ndx){
            const s = this.get(ndx, api.SQLITE_STRING);
            return null===s ? s : JSON.parse(s);
        },
        /**
           Returns the result column name of the given index, or
           throws if index is out of bounds or this statement has been
           finalized. This can be used without having run step()
           first.
        */
        getColumnName: function(ndx){
            return api.sqlite3_column_name(
                affirmColIndex(affirmStmtOpen(this),ndx)._pStmt, ndx
            );
        },
        /**
           If this statement potentially has result columns, this
           function returns an array of all such names. If passed an
           array, it is used as the target and all names are appended
           to it. Returns the target array. Throws if this statement
           cannot have result columns. This object's columnCount member
           holds the number of columns.
        */
        getColumnNames: function(tgt){
            affirmColIndex(affirmStmtOpen(this),0);
            if(!tgt) tgt = [];
            for(let i = 0; i < this.columnCount; ++i){
                tgt.push(api.sqlite3_column_name(this._pStmt, i));
            }
            return tgt;
        },
        /**
           If this statement has named bindable parameters and the
           given name matches one, its 1-based bind index is
           returned. If no match is found, 0 is returned. If it has no
           bindable parameters, the undefined value is returned.
        */
        getParamIndex: function(name){
            return (affirmStmtOpen(this).parameterCount
                    ? api.sqlite3_bind_parameter_index(this._pStmt, name)
                    : undefined);
        }
    }/*Stmt.prototype*/;

    /** OO binding's namespace. */
    const SQLite3 = {
        version: {
            lib: api.sqlite3_libversion(),
            ooApi: "0.0.1"
        },
        DB,
        Stmt,
        /**
           Reports whether a given compile-time option, named by the
           given argument. It has several distinct uses:

           If optName is an array then it is expected to be a list of
           compilation options and this function returns an object
           which maps each such option to true or false, indicating
           whether or not the given option was included in this
           build. That object is returned.

           If optName is an object, its keys are expected to be
           compilation options and this function sets each entry to
           true or false. That object is returned.

           If passed no arguments then it returns an object mapping
           all known compilation options to their compile-time values,
           or boolean true if they are defined with no value.

           In all other cases it returns true if the given option was
           active when when compiling the sqlite3 module, else false.

           Compile-time option names may optionally include their
           "SQLITE_" prefix. When it returns an object of all options,
           the prefix is elided.
        */
        compileOptionUsed: function f(optName){
            if(!arguments.length){
                if(!f._opt){
                    f._rx = /^([^=]+)=(.+)/;
                    f._rxInt = /^-?\d+$/;
                    f._opt = function(opt, rv){
                        const m = f._rx.exec(opt);
                        rv[0] = (m ? m[1] : opt);
                        rv[1] = m ? (f._rxInt.test(m[2]) ? +m[2] : m[2]) : true;
                    };                    
                }
                const rc = {}, ov = [0,0];
                let i = 0, k;
                while((k = api.sqlite3_compileoption_get(i++))){
                    f._opt(k,ov);
                    rc[ov[0]] = ov[1];
                }
                return rc;
            }
            else if(Array.isArray(optName)){
                const rc = {};
                optName.forEach((v)=>{
                    rc[v] = api.sqlite3_compileoption_used(v);
                });
                return rc;
            }
            else if('object' === typeof optName){
                Object.keys(optName).forEach((k)=> {
                    optName[k] = api.sqlite3_compileoption_used(k);
                });
                return optName;
            }
            return (
                'string'===typeof optName
            ) ? !!api.sqlite3_compileoption_used(optName) : false;
        }
    };

    namespace.sqlite3 = {
        api: api,
        SQLite3
    };
});
