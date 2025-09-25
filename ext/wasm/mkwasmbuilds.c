/*
** 2024-09-23
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
** This app's single purpose is to emit parts of the Makefile code for
** sqlite3's canonical WASM build. The main motivation is to generate
** code which "could" be created via GNU Make's eval command but is
** highly illegible when constructed that way. Attempts to write this
** app in Bash and TCL have suffered from the problem that those
** languages require escaping $ symbols, making the resulting script
** code as illegible as the eval spaghetti we want to get away
** from. Maintaining it in C is, somewhat surprisingly, _slightly_
** less illegible than writing it in bash, tcl, or native Make code.
**
** The emitted makefile code is not standalone - it depends on
** variables and $(call)able functions from the main makefile.
*/

#undef NDEBUG
#define DEBUG 1
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define pf printf
#define ps puts

/* Separator to help eyeballs find the different output sections */
#define zBanner \
  "\n########################################################################\n"

/*
** Flags for use with BuildDef::flags.
**
** Maintenance reminder: do not combine flags within this enum,
** e.g. F_BUNDLER_FRIENDLY=0x02|F_ESM, as that will lead
** to breakage in some of the flag checks.
*/
enum {
  /* Indicates an ESM module build. */
  F_ESM              = 0x01,
  /* Indicates a "bundler-friendly" build mode. */
  F_BUNDLER_FRIENDLY = 1<<1,
  /* Indicates that this build is unsupported. Such builds are not
  ** added to the 'all' target. The unsupported builds exist primarily
  ** for experimentation's sake. */
  F_UNSUPPORTED      = 1<<2,
  /* Elide this build from the 'all' target. */
  F_NOT_IN_ALL       = 1<<3,
  /* If it's a 64-bit build. */
  F_64BIT            = 1<<4,
  /* Indicates a node.js-for-node.js build (untested and
  ** unsupported). */
  F_NODEJS           = 1<<5,
  /* Indicates a wasmfs build (untested and unsupported). */
  F_WASMFS           = 1<<6,

  /**
     Which compiled files from $(dir.dout)/buildName/*.{js,mjs,wasm}
     to copy to $(dir.dout) after creating them.
  */
  CP_JS              = 1 << 30,
  CP_WASM            = 1 << 31,
  CP_ALL             = CP_JS | CP_WASM
};

/*
** Info needed for building one concrete JS/WASM combination..
*/
struct BuildDef {
  /**
     Base name of output JS and WASM files.
  */
  const char *zBaseName;
  /*
  ** A glyph to use in log messages for this build, intended to help
  ** the eyes distinguish the build lines more easily in parallel
  **  builds.
  **
  ** The convention for 32- vs 64-bit pairs is to give them similar
  ** emoji, e.g. a cookie for 32-bit and a donut or cake for 64.
  ** Alternately, the same emoji a "64" suffix, excep that that throws
  ** off the output alignment in parallel builds ;).
  */
  const char *zEmo;
  /*
  ** If the build needs its x.wasm renamed in its x.{js,mjs} then this
  ** must hold the base name to rename it to.  Typically "sqlite3" or
  ** "sqlite3-64bit". This is the case for builds which are named
  ** something like sqlite3-foo-bar but can use the vanilla
  ** sqlite3.wasm file. In such cases we don't need the extra
  ** sqlite3-foo-bar.wasm which Emscripten (necessarily) creates when
  ** compiling the module, so we patch (at build-time) the JS file to
  ** use this name instead sqlite3-foo-bar.
  */
  const char *zDotWasm;
  const char *zCmppD;     /* Extra -D... flags for c-pp */
  const char *zEmcc;      /* Extra flags for emcc */
  const char *zEnv;       /* emcc -sENVIRONMENT=... value */
  /*
  ** Makefile code "ifeq (...)". If set, this build is enclosed in a
  ** $zIfCond/endif block.
  */
  const char *zIfCond;    /* makefile "ifeq (...)" or similar */
  int flags;              /* Flags from LibModeFlags */
};
typedef struct BuildDef BuildDef;

/*
** WASM_CUSTOM_INSTANTIATE changes how the JS pieces load the .wasm
** file from the .js file. When set, our JS takes over that step from
** Emscripten. Both modes are functionally equivalent but
** customization gives us access to wasm module state which we don't
** otherwise have. That said, the library also does not _need_ that
** state, so we don't _need_ to customize that step.
*/
#if !defined(WASM_CUSTOM_INSTANTIATE)
#  define WASM_CUSTOM_INSTANTIATE 0
#elif (WASM_CUSTOM_INSTANTIATE+0)==0
#  undef WASM_CUSTOM_INSTANTIATE
#  define WASM_CUSTOM_INSTANTIATE 0
#endif

#if WASM_CUSTOM_INSTANTIATE
/* c-pp -D... flags for the custom instantiateWasm(). */
#define C_PP_D_CUSTOM_INSTANTIATE " -Dcustom-Module.instantiateWasm "
#else
#define C_PP_D_CUSTOM_INSTANTIATE
#endif

/*
** List of distinct library builds. Each one has to be set up in
** oBuildDefs. See the next comment block.
**
** Many makefile vars use these identifiers for naming stuff, e.g.:
**
**  out.NAME.js   = output file NAME.js or NAME.mjs
**  out.NAME.wasm = output file NAME.wasm
**  logtag.NAME   = Used for decorating log output
**
** etc.
***/
#define BuildDefs_map(E)  \
  E(vanilla) E(vanilla64) \
  E(esm)     E(esm64)     \
  E(bundler) E(bundler64) \
  E(node)    E(node64)    \
  E(wasmfs)

/*
** The set of WASM builds for the library (as opposed to the apps
** (fiddle, speedtest1)). Their order in BuildDefs_map is mostly
** insignificant, but some makefile vars used by some builds are set
** up by prior builds. Because of that, the (sqlite3, vanilla),
** (sqlite3, esm), and (sqlite3, bundler-friendly) builds should be
** defined first (in that order).
*/
struct BuildDefs {
#define E(N) BuildDef N;
  BuildDefs_map(E)
#undef E
};
typedef struct BuildDefs BuildDefs;

const BuildDefs oBuildDefs = {
  /*
  ** The canonical build, against which all others are compared and
  ** contrasted.  This is the one we post downloads for.
  */
  .vanilla = {
    /* This one's zBaseName and zEnv MUST be non-NULL so it can be
    ** used as a default for all others. */
    .zEmo        = "🍦",
    .zBaseName   = "sqlite3",
    .zDotWasm    = 0,
    .zCmppD      = 0,
    .zEmcc       = 0,
    .zEnv        = "web,worker",
    .zIfCond     = 0,
    .flags       = CP_ALL
  },

  /* The canonical build in 64-bit. */
  .vanilla64 = {
    .zEmo        = "🍨",
    .zBaseName   = "sqlite3-64bit",
    .zDotWasm    = 0,
    .zCmppD      = 0,
    .zEmcc       = "-sMEMORY64=1 -sWASM_BIGINT=1",
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_ALL | F_64BIT
  },

  /* The canonical esm build. */
  .esm = {
    .zEmo        = "🍬",
    .zBaseName   = "sqlite3",
    .zDotWasm    = 0,
    .zCmppD      = "-Dtarget=es6-module",
    .zEmcc       = 0,
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_JS | F_ESM
  },

  /* The canonical esm build in 64-bit. */
  .esm64 = {
    .zEmo        = "🍫",
    .zBaseName   = "sqlite3-64bit",
    .zDotWasm    = 0,
    .zCmppD      = "-Dtarget=es6-module",
    .zEmcc       = "-sMEMORY64=1 -sWASM_BIGINT=1",
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_JS | F_ESM | F_64BIT
  },

  /*
  ** Core bundler-friendly build. Untested and "not really" supported,
  ** but required by the downstream npm subproject.
  **
  ** Testing these requires special-purpose node-based tools and
  ** custom test apps, none of which we have/use. So we can pass them
  ** off as-is to the npm subproject and they spot failures pretty
  ** quickly ;).
  */
  .bundler = {
    .zEmo        = "👛",
    .zBaseName   = "sqlite3-bundler-friendly",
    .zDotWasm    = "sqlite3",
    .zCmppD      = "$(c-pp.D.esm) -Dtarget=es6-bundler-friendly",
    .zEmcc       = 0,
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_JS | F_BUNDLER_FRIENDLY | F_ESM
    //| F_NOT_IN_ALL
  },

  /* 64-bit bundler-friendly. */
  .bundler64 = {
    .zEmo        = "📦",
    .zBaseName   = "sqlite3-bundler-friendlyu",
    .zDotWasm    = "sqlite3-64bit",
    .zCmppD      = "$(c-pp.D.bundler)",
    .zEmcc       = "-sMEMORY64=1",
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_JS | F_ESM | F_BUNDLER_FRIENDLY | F_64BIT | F_NOT_IN_ALL
  },

  /*
  ** We neither build node builds on a regular basis nor test them at
  ** all. They are fully unsupported. Also, our JS targets only
  ** browsers.
  */
  .node = {
    .zEmo        = "🍟",
    .zBaseName   = "sqlite3-node",
    .zDotWasm    = 0,
    .zCmppD      = "-Dtarget=node $(c-pp.D.bundler)",
    .zEmcc       = 0,
    .zEnv        = "node"
    /* Adding ",node" to the zEnv list for the other builds causes
    ** Emscripten to generate code which confuses node: it cannot
    ** reliably determine whether the build is for a browser or for
    ** node. */,
    .zIfCond     = 0,
    .flags       = CP_ALL | F_UNSUPPORTED | F_NODEJS
  },

  /* 64-bit node. */
  .node64 = {
    .zEmo        = "🍔",
    .zBaseName   = "sqlite3-node-64bit",
    .zDotWasm    = 0,
    .zCmppD      = "-Dtarget=node $(c-pp.D.bundler)",
    .zEmcc       = 0,
    .zEnv        = "node",
    .zIfCond     = 0,
    .flags       = CP_ALL | F_UNSUPPORTED | F_NODEJS | F_64BIT
  },

  /* Entirely unsupported. */
  .wasmfs = {
    .zEmo        = "💿",
    .zBaseName   = "sqlite3-wasmfs",
    .zDotWasm    = 0,
    .zCmppD      = "$(c-pp.D.bundler)",
    .zEmcc       = "-sEXPORT_ES6 -sUSE_ES6_IMPORT_META",
    .zEnv        = 0,
    .zIfCond     = "ifeq (1,$(HAVE_WASMFS))",
    .flags       = CP_ALL | F_UNSUPPORTED | F_WASMFS
  }
};

/*
** Emits common vars needed by the rest of the emitted code (but not
** needed by makefile code outside of these generated pieces).
*/
static void mk_prologue(void){
  /* A 0-terminated list of makefile vars which we expect to have been
  ** set up by this point in the build process. */
  char const * aRequiredVars[] = {
    "dir.top",
    "dir.api", "dir.dout", "dir.tmp",
    "MAKEFILE", "MAKEFILE_LIST",
    /* Fiddle... */
    "dir.fiddle", "dir.fiddle.debug",
    "EXPORTED_FUNCTIONS.fiddle",
    /* Some core JS files... */
    // todo: we don't need these anymore
    "sqlite3.js", "sqlite3.mjs",
    "sqlite3-64bit.js", "sqlite3-64bit.mjs",
    /*"just-testing",*/
    0
  };
  char const * zVar;
  int i;
  ps(zBanner "# Build setup sanity checks...");
  for( i = 0; (zVar = aRequiredVars[i]); ++i ){
    pf("ifeq (,$(%s))\n", zVar);
    pf("  $(error build process error: expecting make var $$(%s) to "
       "have been set up by now)\n", zVar);
    ps("endif");
  }

  ps(zBanner
     /** $1 = build name */
     "b.call.wasm-strip = "
     "echo '[$(emo.b.$(1)) $(out.$(1).wasm)] $(emo.strip) wasm-strip'; "
     "$(bin.wasm-strip) $(out.$(1).wasm)\n"
  );

  pf(zBanner
     "define b.do.emcc\n"
     /* $1 = build name */
     "$(bin.emcc) -o $@ $(emcc_opt_full) $(emcc.flags) "
     "$(emcc.jsflags) -sENVIRONMENT=$(emcc.environment.$(1)) "
     "\t\t$(pre-post.$(1).flags) "
     "\t\t$(emcc.flags.$(1)) "
     "\t\t$(cflags.common) $(cflags.$(1)) "
     "\t\t$(SQLITE_OPT) "
     "\t\t$(cflags.wasm_extra_init) $(sqlite3-wasm.cfiles)\n"
     "endef\n"
  );

  {
    /* b.do.wasm-opt = shell code to run $(1) (source wasm file
    ** name) through $(bin.wasm-opt) */
    const char * zOptFlags =
      /*
      ** Flags for wasm-opt. It has many, many, MANY "passes" options
      ** and the ones which appear here were selected solely on the
      ** basis of trial and error.
      **
      ** All wasm file size savings/costs mentioned below are based on
      ** the vanilla build of sqlite3.wasm with -Oz (our shipping
      ** configuration). Comments like "saves nothing" may not be
      ** technically correct: "nothing" means "some neglible amount."
      **
      ** Note that performance gains/losses are _not_ taken into
      ** account here: only wasm file size.
      */
      "--enable-bulk-memory-opt " /* required */
      "--all-features "           /* required */
      "--post-emscripten "        /* Saves roughly 12kb */
      "--strip-debug "            /* We already wasm-strip, but in
                                  ** case this environment has no
                                  ** wasm-strip... */
      /*
      ** The rest are trial-and-error. See wasm-opt --help and search
      ** for "Optimization passes" to find the full list.
      **
      ** With many flags this gets unusuably slow.
      */
      /*"--converge " saves nothing for the options we're using */
      /*"--dce " saves nothing */
      /*"--directize " saves nothing */
      /*"--gsi " no: requires --closed-world flag, which does not
      ** sound like something we want. */
      /*"--gufa --gufa-cast-all --gufa-optimizing " costs roughly 2kb */
      /*"--heap-store-optimization " saves nothing */
      /*"--heap2local " saves nothing */
      //"--inlining --inlining-optimizing " costs roughly 3kb */
      "--local-cse " /* saves roughly 1kb */
      /*"--once-reduction " saves nothing */
      /*"--remove-memory-init " presumably a performance tweak */
      /*"--remove-unused-names " saves nothing */
      /*"--safe-heap "*/
      /*"--vacuum " saves nothing */
      ;
    ps(zBanner
       "# post-compilation WASM file optimization");

    /* b.do.wasm-opt $1 = build name*/
    ps("ifeq (,$(bin.wasm-opt))"); {
      ps("b.do.wasm-opt = echo '$(logtag.$(1)) wasm-opt not available'");
    }
    ps("else"); {
      ps("define b.do.wasm-opt");
      pf(
        "echo '[$(emo.b.$(1)) $(out.$(1).wasm)] $(emo.wasm-opt) $(bin.wasm-opt)';\\\n"
        "\ttmpfile=$(dir.dout.$(1))/wasm-opt-tmp.$(1).wasm; \\\n"
        "\trm -f $$tmpfile; \\\n"
        "\tif $(bin.wasm-opt) $(out.$(1).wasm) "
        "-o $$tmpfile \\\n"
        "\t\t%s; then \\\n"
        "\t\tmv $$tmpfile $(out.$(1).wasm); \\\n"
#if 0
        "\t\techo -n 'After wasm-opt: '; \\\n"
        "\t\tls -l $(1); \\\n"
#endif
        /* It's very likely that the set of wasm-opt flags varies from
        ** version to version, so we'll ignore any errors here. */
        "\telse \\\n"
        "\t\trm -f $$tmpfile; \\\n"
        "\t\techo '$(2) $(emo.fire) ignoring wasm-opt failure'; \\\n"
        "\tfi\n",
        zOptFlags
      );
      ps("endef");
    }
    ps("endif");
  }

  ps("more: all");
}

/*
** Emits makefile code for setting up values for the --pre-js=FILE,
** --post-js=FILE, and --extern-post-js=FILE emcc flags, as well as
** populating those files. This is necessary for any builds which
** embed the library's JS parts of this build (as opposed to parts
** which do not use the library-level code).
*/
static void mk_pre_post(char const *zBuildName){
/* Very common printf() args combo. */

  pf("%s# Begin --pre/--post flags for %s\n", zBanner, zBuildName);

  ps("# --pre-js=...");
  pf("pre-js.%s.js = $(dir.tmp)/pre-js.%s.js\n",
     zBuildName, zBuildName);

  if( 0==WASM_CUSTOM_INSTANTIATE ){
    pf("$(eval $(call b.eval.c-pp,"
       "%s,"
       "$(pre-js.in.js),"
       "$(pre-js.%s.js),"
       "$(c-pp.D.%s)"
       "))",
       zBuildName, zBuildName, zBuildName);
  }else{
#if 0
    fixme;
    /* This part is needed for builds which have to rename the wasm file
       in zDotWasm so that the loader can find it. */
    pf("pre-js.%s.js.intermediary = "
       "$(dir.tmp)/pre-js.%s.intermediary.js\n",
       zBuildName, zBuildName);
    pf("$(eval $(call SQLITE.CALL.C-PP.FILTER,$(pre-js.in.js),"
       "$(pre-js.%s.js.intermediary),"
       C_PP_D_CUSTOM_INSTANTIATE "$(c-pp.D.%s)))\n",
       zBuildName, zBuildName);
    pf("$(pre-js.%s.js): $(pre-js.%s.js.intermediary)\n",
       zBuildName, zBuildName);
    pf("\tcp $(pre-js.%s.js.intermediary) $@\n", zBuildName);
    pf("\t@echo 'sIMS.wasmFilename = \"$(out.%s.wasm)\";' >> $@\n",
       zBuildName)
      /* see api/pre-js.c-pp.js:Module.instantiateModule() */;
#endif
  }

  ps("\n# --post-js=...");
  pf("post-js.%s.js = $(dir.tmp)/post-js.%s.js\n",
     zBuildName, zBuildName);
  pf("post-js.%s.in ="
     " $(dir.api)/post-js-header.js"
     " $(sqlite3-api.%s.js)"
     " $(dir.api)/post-js-footer.js\n",
     zBuildName, zBuildName);

  pf("$(eval $(call b.eval.c-pp,"
     "%s,"
     "$(post-js.%s.in),"
     "$(post-js.%s.js),"
     "$(c-pp.D.%s)"
     "))\n",
     zBuildName, zBuildName, zBuildName, zBuildName);

  pf("$(post-js.%s.js): $(post-js.%s.in)\n",
     zBuildName, zBuildName);

  ps("\n# --extern-post-js=...");
  pf("extern-post-js.%s.js = $(dir.tmp)/extern-post-js.%s.js\n",
     zBuildName, zBuildName);
  pf("$(eval $(call b.eval.c-pp,"
     "%s,"
     "$(extern-post-js.in.js),"
     "$(extern-post-js.%s.js),"
     C_PP_D_CUSTOM_INSTANTIATE "$(c-pp.D.%s)))\n",
     zBuildName, zBuildName, zBuildName);

  ps("\n# --pre/post misc...");
  /* Combined flags for use with emcc... */
  pf("pre-post.%s.flags = "
     "--extern-pre-js=$(sqlite3-license-version.js) "
     "--pre-js=$(pre-js.%s.js) "
     "--post-js=$(post-js.%s.js) "
     "--extern-post-js=$(extern-post-js.%s.js)\n",
     zBuildName, zBuildName, zBuildName, zBuildName);


  /* Set up deps... */
  pf("pre-post.%s.deps = "
     "$(pre-post-jses.common.deps) "
     "$(post-js.%s.js) $(extern-post-js.%s.js) "
     "$(dir.tmp)/pre-js.%s.js\n",
     zBuildName, zBuildName, zBuildName, zBuildName);
  pf("# End --pre/--post flags for %s%s", zBuildName, zBanner);
}

static char const * BuildDef_jsext(const BuildDef * pB){
  return (F_ESM & pB->flags) ? ".mjs" : ".js";
}

static char const * BuildDef_basename(const BuildDef * pB){
  return pB->zBaseName ? pB->zBaseName : oBuildDefs.vanilla.zBaseName;
}

static void emit_compile_start(char const *zBuildName){
  pf("\t@$(call b.call.mkdir@);"
     " $(call b.echo,%s,$(emo.compile) building ...)\n",
     zBuildName);
}

/**
   Emit rules for sqlite3-api.${zBuildName}.js.  zCmppD is optional
   flags for $(bin.c-pp).
*/
static void emit_api_js(char const *zBuildName,
                        char const *zCmppD){
  pf("c-pp.D.%s = %s\n"
     "sqlite3-api.%s.js = $(dir.tmp)/sqlite3-api.%s.js\n",
     zBuildName, zCmppD ? zCmppD: "",
     zBuildName, zBuildName);
  pf("$(eval $(call b.eval.c-pp,"
     "%s,"
     "$(sqlite3-api.jses),"
     "$(sqlite3-api.%s.js),"
     "$(c-pp.D.%s)"
     "))\n",
     zBuildName, zBuildName, zBuildName);
  pf("$(out.%s.js): $(sqlite3-api.%s.js)\n",
     zBuildName, zBuildName);
}

/*
** Emits makefile code for one build of the library.
*/
static void mk_lib_mode(const char *zBuildName, const BuildDef * pB){
  const char * zJsExt = BuildDef_jsext(pB);
  char const * const zBaseName = BuildDef_basename(pB);

  assert( oBuildDefs.vanilla.zEnv );
  assert( zBaseName );

  pf("%s# Begin build [%s%s]. flags=0x%02x\n", zBanner,
     pB->zEmo, zBuildName, pB->flags);
  pf("# zCmppD=%s\n# zBaseName=%s\n",
     pB->zCmppD ? pB->zCmppD : "", zBaseName);

  pf(
    "b.names += %s\n"
    "emo.b.%s = %s\n",
    zBuildName, zBuildName, pB->zEmo);

  pf("logtag.%s ?= [%s [%s] $@]:\n", zBuildName, pB->zEmo, zBuildName);
  if( pB->zIfCond ){
    pf("%s\n", pB->zIfCond );
  }
  pf("$(info $(logtag.%s) Setting up target b-%s)\n",
     zBuildName, zBuildName );

  pf("dir.dout.%s ?= $(dir.dout)/%s\n", zBuildName, zBuildName);
  pf("out.%s.base ?= $(dir.dout.%s)/%s\n",
     zBuildName, zBuildName, zBaseName);
  pf("out.%s.js ?= $(dir.dout.%s)/%s%s\n",
     zBuildName, zBuildName, zBaseName, zJsExt);
  pf("out.%s.wasm ?= $(dir.dout.%s)/%s.wasm\n",
     //"$(basename $@).wasm"
     zBuildName, zBuildName, zBaseName);


  pf("dir.dout.%s ?= $(dir.dout)/%s\n", zBuildName, zBuildName);
  pf("out.%s.base ?= $(dir.dout.%s)/%s\n",
     zBuildName, zBuildName, zBaseName);
  pf("out.%s.js ?= $(dir.dout.%s)/%s%s\n",
     zBuildName, zBuildName, zBaseName, zJsExt);
  pf("out.%s.wasm ?= $(dir.dout.%s)/%s.wasm\n",
     //"$(basename $@).wasm"
     zBuildName, zBuildName, zBaseName);
  pf("c-pp.D.%s ?= %s\n", zBuildName, pB->zCmppD ? pB->zCmppD : "");
  pf("emcc.environment.%s ?= %s\n", zBuildName,
     pB->zEnv ? pB->zEnv : oBuildDefs.vanilla.zEnv);
  pf("emcc.flags.%s ?= %s\n", zBuildName, pB->zEmcc ? pB->zEmcc : "");

  emit_api_js(zBuildName, pB->zCmppD);
  if( pB->flags & F_64BIT ){
    pf("c-pp.D.%s +=  $(c-pp.D.64bit)\n", zBuildName);
  }
  mk_pre_post(zBuildName);

  { /* build it... */
    pf(zBanner
       "$(out.%s.js): $(MAKEFILE_LIST) $(sqlite3-wasm.cfiles)"
       " $(EXPORTED_FUNCTIONS.api)"
       " $(bin.mkwb) $(pre-post.%s.deps)"
       "\n",
       zBuildName, zBuildName);

    emit_compile_start(zBuildName);

    if( F_UNSUPPORTED & pB->flags ){
      pf("\t@echo '$(logtag.%s) $(emo.fire)$(emo.fire)$(emo.fire): "
         "unsupported build. Use at your own risk.'\n", zBuildName);
    }
    pf("\t$(b.cmd@)$(call b.do.emcc,%s)\n", zBuildName);

    { /* Post-compilation transformations and copying to
         $(dir.dout)... */
      if( (F_ESM & pB->flags) || (F_NODEJS & pB->flags) ){
        pf("\t@$(call b.call.patch-export-default,1,%d,$(logtag.%s))\n",
           (F_WASMFS & pB->flags) ? 1 : 0,
           zBuildName
        );
      }

      pf("\t@chmod -x $(out.%s.wasm)\n", zBuildName
         /* althttpd will automatically try to execute wasm files
            if they have the +x bit set. Why that bit is set
            at all is a mystery. */);
      pf("\t@$(call b.call.wasm-strip,%s)\n", zBuildName);

      pf("\t@$(call b.do.wasm-opt,%s)\n", zBuildName);
      pf("\t@$(call b.call.strip-js-cruft,$(logtag.%s))\n", zBuildName);

      if( CP_JS & pB->flags && !(pB->zDotWasm/*handled below*/) ){
        pf("\t@$(call b.call.cp,%s,$@,$(dir.dout))\n",
           zBuildName
        );
      }
      if( CP_WASM & pB->flags ){
        pf("\t@$(call b.call.cp,%s,$(basename $@).wasm,$(dir.dout))\n",
           zBuildName
           //"\techo '[%s $(out.%s.wasm)] $(emo.disk) $(dir.dout)/$(notdir $(out.%s.wasm))'
           //pB->zEmo, zBuildName
        );
      }
      /*
      ** $(bin.emcc) will write out $@ and will create a like-named
      ** .wasm file. The resulting .wasm and .js/.mjs files are
      ** identical across all builds which have the same pB->zEmcc.
      **
      ** We copy one or both of those files to $(dir.dout) (the top-most
      ** build target dir), but: that .wasm file name gets hard-coded
      ** into $@ so we need, for some cases, to patch the name to
      ** pB->zDotWasm when copying to $(dir.dout).
      */
      if( pB->zDotWasm && (CP_JS & pB->flags) ){
        pf("\t@echo '$(logtag.%s) $(emo.disk) "
           "s/\"%s.wasm\"/\"%s.wasm\"/g "
           "in $(dir.dout)/$(notdir $@)'; \\\n"
           "sed "
           "-e 's/\"%s.wasm\"/\"%s.wasm\"/g' "
           "-e \"s/'%s.wasm'/'%s.wasm'/g\" "
           "$@ > $(dir.dout)/$(notdir $@)\n",
           zBuildName,
           zBaseName, pB->zDotWasm,
           zBaseName, pB->zDotWasm,
           zBaseName, pB->zDotWasm);
      }

      /* Avoid a 3rd occurrence of the bug fixed by 65798c09a00662a3,
      ** which was (in two cases) caused by makefile refactoring and
      ** not recognized until after a release was made with the broken
      ** sqlite3-bundler-friendly.mjs (which is used by the npm
      ** subproject but is otherwise untested/unsupported): */
      pf("\t@if grep -e '^ *importScripts(' $@; "
         "then echo '$(logtag.%s) $(emo.bug)$(emo.fire): "
         "bug fixed in 65798c09a00662a3 has re-appeared'; "
         "exit 1; fi;\n", zBuildName);
    }
  }
  pf("\t@$(call b.echo,%s,$(emo.done) done!)\n", zBuildName);

  pf("\n%dbit: $(out.%s.js)\n"
     "$(out.%s.wasm): $(out.%s.js)\n"
     "b-%s: $(out.%s.js) $(out.%s.wasm)\n",
     (F_64BIT & pB->flags) ? 64 : 32, zBuildName,
     zBuildName, zBuildName,
     zBuildName, zBuildName, zBuildName);

  if( CP_JS & pB->flags ){
    pf("$(dir.dout)/%s%s: $(out.%s.js)\n",
       pB->zBaseName, zJsExt, zBuildName
    );
  }
  if( CP_WASM & pB->flags ){
    pf("$(dir.dout)/%s.wasm: $(out.%s.wasm)\n",
       pB->zBaseName, zBuildName
    );
  }

  pf("%s: $(out.%s.js)\n",
     0==((F_UNSUPPORTED | F_NOT_IN_ALL) & pB->flags)
     ? "all" : "more", zBuildName);

  if( pB->zIfCond ){
    pf("else\n"
       "$(info $(logtag.%s) $(emo.stop) disabled by condition: %s)\n"
       "endif\n",
       zBuildName, pB->zIfCond);
  }
  pf("# End build [%s]%s", zBuildName, zBanner);
}


static void emit_gz(char const *zBuildName,
                    char const *zOutDotSExt){
  pf("\n$(out.%s.%s).gz: $(out.%s.%s)\n"
     "\t@$(call b.echo,%s,$(emo.disk))\n"
     "\t@gzip < $< > $@\n",
     zBuildName, zOutDotSExt,
     zBuildName, zOutDotSExt,
     zBuildName);
}

/*
** Emits rules for the fiddle builds.
*/
static void mk_fiddle(void){
  for(int i = 0; i < 2; ++i ){
    /* 0==normal, 1==debug */
    int const isDebug = i>0;
    const char * const zBuildName = i ? "fiddle.debug" : "fiddle";

    pf(zBanner "# Begin build %s\n", zBuildName);
    if( isDebug ){
      pf("emo.%s = $(emo.fiddle)$(emo.bug)\n", zBuildName);
    }else{
      pf("emo.fiddle = 🎻\n");
    }
    pf("logtag.%s = [$(emo.%s) [%s] $@]:\n",
       zBuildName, zBuildName, zBuildName);
    pf("$(info $(logtag.%s) Setting up target b-%s)\n",
       zBuildName, zBuildName );

    pf("dir.%s = %s\n"
       "out.%s.js = $(dir.%s)/fiddle-module.js\n"
       "out.%s.wasm = $(dir.%s)/fiddle-module.wasm\n"
       "$(out.%s.wasm): $(out.%s.js)\n",
       zBuildName, zBuildName,
       zBuildName, zBuildName,
       zBuildName, zBuildName,
       zBuildName, zBuildName);

    emit_api_js(zBuildName, 0);
    mk_pre_post(zBuildName);

    {/* emcc */
      pf("$(out.%s.js): $(MAKEFILE_LIST) $(MAKEFILE.fiddle) "
         "$(EXPORTED_FUNCTIONS.fiddle) "
         "$(fiddle.cses) "
         "$(pre-post.%s.deps)\n",
         zBuildName, zBuildName);
      emit_compile_start(zBuildName);
      pf("\t$(b.cmd@)$(bin.emcc) -o $@"
         " $(emcc.flags.%s)" /* set in fiddle.make */
         " $(pre-post.%s.flags)"
         " $(fiddle.cses)"
         "\n",
         zBuildName, zBuildName);
      pf("\t@chmod -x $(out.%s.wasm)\n", zBuildName);
      pf("\t@$(call b.call.wasm-strip,%s)\n", zBuildName);
      pf("\t@$(call b.call.strip-js-cruft,$(logtag.%s))\n", zBuildName);
      pf("\t@$(call b.call.cp,"
         "%s,"
         "$(dir.api)/sqlite3-opfs-async-proxy.js,"
         "$(dir $@))\n", zBuildName);
      if( isDebug ){
        pf("\t@$(call b.call.cp,%s,"
           "$(dir.fiddle)/index.html "
           "$(dir.fiddle)/fiddle.js "
           "$(dir.fiddle)/fiddle-worker.js,"
           "$(dir $@)"
           ")\n",
           zBuildName);
      }
    }

    pf("\n%s: $(out.%s.wasm)\n", isDebug ? "more" : "all", zBuildName);

    /* Compress fiddle files. We handle each file separately, rather
       than compressing them in a loop in the previous target, to help
       avoid that hand-edited files, like fiddle-worker.js, do not end
       up with stale .gz files (which althttpd will then serve instead
       of the up-to-date uncompressed one). */
    emit_gz(zBuildName, "js");
    emit_gz(zBuildName, "wasm");

    pf("\n%s: $(out.%s.js).gz $(out.%s.wasm).gz\n"
       "b-%s: %s\n",
       zBuildName, zBuildName, zBuildName,
       zBuildName, zBuildName);
    if( isDebug ){
      ps("fiddle-debug: fiddle.debug");
    }else{
      ps("all: b-fiddle");
    }
    pf("# End %s" zBanner, zBuildName);
  }
}

static void mk_speedtest1(void){
  char const *zBuildName = "speedtest1";
  pf(zBanner "# Begin build %s\n", zBuildName);
  pf("emo.%s ="
     "🛼" // roller skates
     /*"🏎" //racecar doesn't show up well in my emacs or terminal */
     "\n",
     zBuildName);
  pf("logtag.%s = [$(emo.%s) [%s] $@]:\n"
     "$(info $(logtag.%s) Setting up target speedtest1)\n"
     "all: %s\n",
     zBuildName, zBuildName, zBuildName,
     zBuildName, zBuildName );

  pf("dir.dout.%s ?= $(dir.dout)\n", zBuildName);
  pf("out.%s.js = $(dir.dout)/speedtest1.js\n"
     "out.%s.wasm = $(dir.dout)/speedtest1.wasm\n",
     zBuildName, zBuildName);

  emit_api_js(zBuildName, 0);
  mk_pre_post(zBuildName);

  /* Main rules are in the makefile */

#if 0
  mk_pre_post("speedtest1-vanilla");
  ps(zBanner "ifeq (1,$(HAVE_WASMFS))");
  mk_pre_post("speedtest1-wasmfs");
  ps("endif\n# ^^^ HAVE_WASMFS" zBanner);
#endif
#if 0
  mk_pre_post(0, "speedtest1","vanilla", 0, "speedtest1.wasm");
  mk_pre_post(0, "speedtest1-wasmfs", "esm",
              "$(c-pp.D.sqlite3-bundler-friendly) -Dwasmfs",
              "speetest1-wasmfs.wasm");
#endif

}

int main(void){
  int rc = 0;
  const BuildDef *pB;
  pf("# What follows was GENERATED by %s. Edit at your own risk.\n", __FILE__);
  mk_prologue();
#define E(N) mk_lib_mode(# N, &oBuildDefs.N);
  BuildDefs_map(E)
#undef E
  pf(zBanner
     "$(dir.dout)/sqlite3.js: $(out.vanilla.js)\n"
     "$(dir.dout)/sqlite3.mjs: $(out.esm.js)\n"
     "$(dir.dout)/sqlite3.wasm: $(out.vanilla.wasm)\n"
     "$(dir.dout)/sqlite3-64bit.js: $(out.vanilla64.js)\n"
     "$(dir.dout)/sqlite3-64bit.mjs: $(out.esm64.js)\n"
     "$(dir.dout)/sqlite3-64bit.wasm: $(out.vanilla64.wasm)\n"
     "b-vanilla: $(dir.dout)/sqlite3.wasm\n"
     "b-vanilla64: $(dir.dout)/sqlite3-64bit.wasm\n"
     "b-esm: $(dir.dout)/sqlite3.mjs\n"
     "b-esm64: $(dir.dout)/sqlite3-64bit.mjs\n"
  );
  mk_fiddle();
  mk_speedtest1();
  return rc;
}
