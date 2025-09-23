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
** Flags for use with BuildDef::flags and the 3rd argument to
** mk_pre_post().
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
  /**
     A glyph to use in log messages for this build, intended to help
     the eyes distinguish the build lines more easily in parallel
     builds.
  */
  const char *zEmo;
  /**
     If the build needs its x.wasm renamed in its x.{js,mjs}
     then this must hold the base name to rename it to.
     Typically "sqlite3" or "sqlite3-64bit".
  */
  const char *zDotWasm;
  const char *zCmppD;     /* Extra -D... flags for c-pp */
  const char *zEmcc;      /* Extra flags for emcc */
  const char *zEnv;       /* emcc -sENVIRONMENT=X flag */
  const char *zIfCond;    /* "ifeq (...)" or similar */
  int flags;              /* Flags from LibModeFlags */
};
typedef struct BuildDef BuildDef;

#if !defined(WASM_CUSTOM_INSTANTIATE)
#  define WASM_CUSTOM_INSTANTIATE 0
#elif (WASM_CUSTOM_INSTANTIATE+0)==0
#  undef WASM_CUSTOM_INSTANTIATE
#  define WASM_CUSTOM_INSTANTIATE 0
#endif

#if WASM_CUSTOM_INSTANTIATE
#define C_PP_D_CUSTOM_INSTANTIATE " -Dcustom-Module.instantiateWasm "
#else
#define C_PP_D_CUSTOM_INSTANTIATE
#endif

/* List of distinct library builds. See next comment block. */
#define BuildDefs_map(E)  \
  E(vanilla) E(vanilla64) \
  E(esm) E(esm64)         \
  E(bundler) E(bundler64) \
  E(node) E(node64) \
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
  .vanilla = {
    /* This one's zBaseName and zEnv MUST be non-NULL so it can be
       used as a default for all others. */
    .zEmo        = "🍦",
    .zBaseName   = "sqlite3",
    .zDotWasm    = 0,
    .zCmppD      = 0,
    .zEmcc       = 0,
    .zEnv        = "web,worker",
    .zIfCond     = 0,
    .flags       = CP_ALL
  },

  .vanilla64 = {
    .zEmo        = "🍨",
    .zBaseName   = "sqlite3-64bit",
    .zDotWasm    = 0,
    .zCmppD      = 0,
    .zEmcc       = "-sMEMORY64=1",
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_ALL | F_64BIT// | F_NOT_IN_ALL
  },

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

  .esm64 = {
    .zEmo        = "🍫",
    .zBaseName   = "sqlite3-64bit",
    .zDotWasm    = 0,
    .zCmppD      = "-Dtarget=es6-module",
    .zEmcc       = "-sMEMORY64=1",
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_JS | F_ESM | F_64BIT // | F_NOT_IN_ALL
  },

  .bundler = {
    /* Core bundler-friendly build. Untested and "not really"
    ** supported, but required by the downstream npm subproject.
    ** Testing these would require special-purpose node-based tools and
    ** custom test apps. Or we can pass them off as-is to the npm
    ** subproject and they spot failures pretty quickly ;). */
    .zEmo        = "👛",
    .zBaseName   = "sqlite3-bundler-friendly",
    .zDotWasm    = "sqlite3",
    .zCmppD      = "$(c-pp.D.esm) -Dtarget=es6-bundler-friendly",
    .zEmcc       = 0,
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_JS | F_BUNDLER_FRIENDLY | F_ESM
  },

  .bundler64 = {
    .zEmo        = "📦",
    .zBaseName   = "sqlite3-bundler-friendly",
    .zDotWasm    = 0,
    .zCmppD      = "$(c-pp.D.bundler)",
    .zEmcc       = "-sMEMORY64=1",
    .zEnv        = 0,
    .zIfCond     = 0,
    .flags       = CP_JS | F_ESM | F_BUNDLER_FRIENDLY | F_64BIT
  },

  /* We neither build nor test node builds on a regular basis. They
     are fully unsupported. */
  .node = {
    .zEmo        = "🍟",
    .zBaseName   = "sqlite3-node",
    .zDotWasm    = 0,
    .zCmppD      = "-Dtarget=node $(c-pp.D.bundler)",
    .zEmcc       = 0,
    .zEnv        = "node",
    /*
      Adding ",node" to the list for the other builds causes
      Emscripten to generate code which confuses node: it cannot
      reliably determine whether the build is for a browser or for
      node.  */
    .zIfCond     = 0,
    .flags       = CP_ALL | F_UNSUPPORTED | F_NODEJS
  },

  /* Entirely unsupported. */
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
    "dir.fiddle", "dir.fiddle-debug",
    "MAKEFILE.fiddle",
    "EXPORTED_FUNCTIONS.fiddle",
    /* Some core JS files... */
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
     "emoji ?= 1\n"
     "ifneq (0,$(emoji))\n"
     "emo.disk = 💾\n"
     "emo.fire = 🔥\n"
     "emo.done = 🏆\n" /*🏁*/
     "emo.bug = 🐞\n"
     "emo.megaphone = 📣\n"
     "emo.mute = 🔇\n"
     "emo.tool = 🔨\n"
     "emo.compile = ⏳\n"
     "emo.info =\n"
     "emo.stop =🛑\n"
     "emo.strip =🪚\n" /*🔪*/
     "emo.garbage =🗑\n"
     "endif\n"
  );

  ps(zBanner
     /** $1 = build name
         $2 = Input file(s): cat $(2) | c-pp ...
         $3 = Output file: c-pp -o $(3)
         $4 = optional c-pp -D... flags */
     "define b.do.c-pp\n"
     "$(3): $$(MAKEFILE_LIST) $$(bin.c-pp) $(2)\n"
     "\t@echo '$$(logtag.$(1)) $$(emo.disk) $(4)'; mkdir -p $$(dir $$@)\n"
     "\t@cat $(2) | $$(bin.c-pp) -o $(3) $(4) $$(SQLITE.CALL.C-PP.FILTER.global) || exit $$$$?\n"
     "\nCLEAN_FILES += $(2)\n"
     "endef\n"
  );

  ps(zBanner
     /** $1 = build name */
     "b.do.wasm-strip = "
     "echo '$(logtag.$(1)) $(emo.strip) wasm-strip $(out.$(1).wasm)'; "
     "$(bin.wasm-strip) $(out.$(1).wasm)\n"
  );

  ps(zBanner
     "# Inputs for the sqlite3-api.js family.\n"
     "#\n"
     "# sqlite3-license-version.js = generated JS file with the license\n"
     "# header and version info.\n"
     "sqlite3-license-version.js = $(dir.tmp)/sqlite3-license-version.js\n"
     "# $(sqlite3-api-build-version.js) = generated JS file which populates the\n"
     "# sqlite3.version object using $(bin.version-info).\n"
     "sqlite3-api-build-version.js = $(dir.tmp)/sqlite3-api-build-version.js\n"
     "# sqlite3-api.jses = the list of JS files which make up\n"
     "# $(sqlite3-api.js.in), in the order they need to be assembled.\n"
     "sqlite3-api.jses = $(sqlite3-license-version.js)\n"
     "# sqlite3-api-prologue.js: initial bootstrapping bits:\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-api-prologue.js\n"
     "# whwhasm.js and jaccwabyt.js: Low-level utils, mostly replacing\n"
     "# Emscripten glue:\n"
     "sqlite3-api.jses += $(dir.common)/whwasmutil.js\n"
     "sqlite3-api.jses += $(dir.jacc)/jaccwabyt.js\n"
     "# sqlite3-api-glue Glues the previous part together with sqlite:\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-api-glue.c-pp.js\n"
     "sqlite3-api.jses += $(sqlite3-api-build-version.js)\n"
     "# sqlite3-api-oo1 = the oo1 API:\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-api-oo1.c-pp.js\n"
     "# sqlite3-api-worker = the Worker1 API:\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-api-worker1.c-pp.js\n"
     "# sqlite3-vfs-helper = helper APIs for VFSes:\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-vfs-helper.c-pp.js\n"
     "ifeq (0,$(wasm-bare-bones))\n"
     "  # sqlite3-vtab-helper = helper APIs for VTABLEs:\n"
     "  sqlite3-api.jses += $(dir.api)/sqlite3-vtab-helper.c-pp.js\n"
     "endif\n"
     "# sqlite3-vfs-opfs = the first OPFS VFS:\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-vfs-opfs.c-pp.js\n"
     "# sqlite3-vfs-opfs-sahpool = the second OPFS VFS:\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-vfs-opfs-sahpool.c-pp.js\n"
     "# sqlite3-api-cleanup.js = \"finalizes\" the build and cleans up\n"
     "# any extraneous global symbols which are needed temporarily\n"
     "# by the previous files.\n"
     "sqlite3-api.jses += $(dir.api)/sqlite3-api-cleanup.js"
  );

  ps(zBanner
     "# $(sqlite3-license-version.js) contains the license header and\n"
     "# in-comment build version info.\n"
     "#\n"
     "# Maintenance reminder: there are awk binaries out there which do not\n"
     "# support -e SCRIPT.\n"
     "$(sqlite3-license-version.js): $(MKDIR.bld) $(sqlite3.h) "
     "$(dir.api)/sqlite3-license-version-header.js $(MAKEFILE)\n"
     "\t@echo '$(emo.disk) $@'; { \\\n"
     "\t\tcat $(dir.api)/sqlite3-license-version-header.js;  \\\n"
     "\t\techo '/*'; \\\n"
     "\t\techo '** This code was built from sqlite3 version...'; \\\n"
     "\t\techo '**'; \\\n"
     "\t\tawk '/define SQLITE_VERSION/{$$1=\"\"; print \"**\" $$0}' $(sqlite3.h); \\\n"
     "\t\tawk '/define SQLITE_SOURCE_ID/{$$1=\"\"; print \"**\" $$0}' $(sqlite3.h); \\\n"
     "\t\techo '**'; \\\n"
     "\t\techo '** with the help of Emscripten SDK version $(emcc.version).'; \\\n"
     "\t\techo '*/'; \\\n"
     "\t} > $@"
  );

  ps(zBanner
     "# $(sqlite3-api-build-version.js) injects the build version info into\n"
     "# the bundle in JSON form.\n"
     "$(sqlite3-api-build-version.js): $(MKDIR.bld) $(bin.version-info) $(MAKEFILE)\n"
     "\t@echo '$(emo.disk) $@'; { \\\n"
     "\t\techo 'globalThis.sqlite3ApiBootstrap.initializers.push(function(sqlite3){'; \\\n"
     "\t\techo -n '  sqlite3.version = '; \\\n"
     "\t\t$(bin.version-info) --json; \\\n"
     "\t\techo ';'; \\\n"
     "\t\techo '});'; \\\n"
     "\t} > $@"
  );

  ps(zBanner
     "# extern-post-js* and extern-pre-js* are files for use with\n"
     "# Emscripten's --extern-pre-js and --extern-post-js flags.\n"
     "extern-pre-js.js = $(dir.api)/extern-pre-js.js\n"
     "extern-post-js.in.js = $(dir.api)/extern-post-js.c-pp.js\n"
     "# Emscripten flags for --[extern-][pre|post]-js=... for the\n"
     "# various builds.\n"
     "# pre-post-jses.*.deps = lists of dependencies for the\n"
     "# --[extern-][pre/post]-js files.\n"
     "pre-post-jses.common.deps = "
     "$(extern-pre-js.js) $(sqlite3-license-version.js)"
  );

  ps(zBanner
     "loud ?= 0\n"
     "ifeq (1,$(loud))\n"
     "$(info $(emo.megaphone) Emitting loud build info."
     " Pass loud=0 to disable it.)\n"
     "cmd.loud =\n"
     "else\n"
     "$(info $(emo.mute) Eliding loud build info."
     " Pass emcc.loud=1 to enable it.)\n"
     "cmd.loud = @\n"
     "endif\n"
  );

  ps(zBanner
     "# --post-js and --pre-js are emcc flags we use to append/prepend JS to\n"
     "# the generated emscripten module file. These rules set up the core\n"
     "# pre/post files for use by the various builds. --pre-js is used to\n"
     "# inject code which needs to run as part of the pre-WASM-load phase.\n"
     "# --post-js injects code which runs after the WASM module is loaded\n"
     "# and includes the entirety of the library plus some\n"
     "# Emscripten-specific post-bootstrapping code.\n"
     "pre-js.in.js = $(dir.api)/pre-js.c-pp.js\n"
     "post-js.in.js = $(dir.tmp)/post-js.c-pp.js\n"
     "post-jses.js = $(dir.api)/post-js-header.js $(sqlite3-api.js.in) $(dir.api)/post-js-footer.js\n"
     "$(post-js.in.js): $(MKDIR.bld) $(post-jses.js) $(MAKEFILE)\n"
     "	@echo '$(emo.disk) $@'\n"
     "	@for i in $(post-jses.js); do \\n"
     "		echo \"/* BEGIN FILE: $$i */\"; \\n"
     "		cat $$i; \\n"
     "		echo \"/* END FILE: $$i */\"; \\n"
     "	done > $@\n"
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
    /* SQLITE.CALL.WASM-OPT = shell code to run $(1) (source wasm file
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
    ps("ifeq (,$(bin.wasm-opt))");
    {
      ps("define SQLITE.CALL.WASM-OPT");
      ps("echo 'wasm-opt not available for $(1)'");
      ps("endef");
    }
    ps("else");
    {
      ps("define SQLITE.CALL.WASM-OPT"
         /* $1 = build name, $2 = log message prefix*/
      );
      pf(
        "echo '$(2) $(emo.tool) Applying $(bin.wasm-opt)';\\\n"
        "\trm -f wasm-opt-tmp.$(1).wasm;\\\n"
        /* It's very likely that the set of wasm-opt flags varies from
        ** version to version, so we'll ignore any errors here. */
        "\tif $(bin.wasm-opt) $(out.$(1).wasm) -o wasm-opt-tmp.$(1).wasm \\\n"
        "\t\t%s; then \\\n"
        "\t\tmv wasm-opt-tmp.$(1).wasm $(out.$(1).wasm); \\\n"
#if 0
        "\t\techo -n 'After wasm-opt: '; \\\n"
        "\t\tls -l $(1); \\\n"
#endif
        "\telse \\\n"
        "\t\techo '$(2) $(emo.fire) ignoring wasm-opt failure'; \\\n"
        "\tfi\n",
        zOptFlags
      );
      ps("endef");
    }
    ps("endif");
  }
}

/*
** Emits makefile code for setting up values for the --pre-js=FILE,
** --post-js=FILE, and --extern-post-js=FILE emcc flags, as well as
** populating those files.
*/
static void mk_pre_post(char const *zBuildName){
/* Very common printf() args combo. */

  pf("%s# Begin --pre/--post flags for %s\n", zBanner, zBuildName);

  ps("\n# --pre-js=...");
  pf("pre-js.%s.js = $(dir.tmp)/pre-js.%s.js\n"
     "CLEAN_FILES += $(pre-js.%s.js)\n"
     "$(pre-js.%s.js): "
     "$(MAKEFILE_LIST) $(sqlite3-license-version.js)\n",
     zBuildName, zBuildName,
     zBuildName,
     zBuildName);

  if( 0==WASM_CUSTOM_INSTANTIATE ){
    pf("\t@echo '$(logtag.%s) $(emo.disk) $(c-pp.D.%s)'; "
       "cat $(pre-js.in.js) | $(bin.c-pp) -o $@ $(c-pp.D.%s)\n",
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
  pf("post-jses.%s = "
     "$(dir.api)/post-js-header.js "
     "$(sqlite3-api.%s.js) "
     "$(dir.api)/post-js-footer.js\n",
     zBuildName, zBuildName);
  pf("$(eval $(call b.do.c-pp,"
     "%s,"
     "$(post-jses.%s),"
     "$(post-js.%s.js),"
     "$(c-pp.D.%s)"
     "))\n",
     zBuildName, zBuildName, zBuildName, zBuildName);

  ps("\n# --extern-post-js=...");
  pf("extern-post-js.%s.js = $(dir.tmp)/extern-post-js.%s.js\n",
     zBuildName, zBuildName);
  pf("$(eval $(call b.do.c-pp,"
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

static void emit_core_vars(char const *zBuildName,
                           const BuildDef * pB){
  const char * zJsExt = BuildDef_jsext(pB);
  char const * const zBaseName = BuildDef_basename(pB);
  pf("dir.dout.%s ?= $(dir.dout)/%s\n", zBuildName, zBuildName);
  pf("out.%s.base ?= $(dir.dout.%s)/%s\n",
     zBuildName, zBuildName, zBaseName);
  pf("out.%s.js ?= $(dir.dout.%s)/%s%s\n",
     zBuildName, zBuildName, zBaseName, zJsExt);
  pf("out.%s.wasm ?= $(dir.dout.%s)/%s.wasm\n",
     //"$(basename $@).wasm"
     zBuildName, zBuildName, zBaseName);
}

/*
** Emits makefile code for one build of the library.
*/
void mk_lib_mode(const char *zBuildName, const BuildDef * pB){
    /* The various targets named X.js or X.mjs also generate X.wasm,
    ** and we need that part of the name to perform some
    ** post-processing after Emscripten generates X.wasm. */;
  const char * zJsExt = BuildDef_jsext(pB);
  char const * const zBaseName = BuildDef_basename(pB);

  assert( oBuildDefs.vanilla.zEnv );
  assert( zBaseName );

  pf("%s# Begin build [%s%s]. flags=0x%02x\n", zBanner,
     pB->zEmo, zBuildName, pB->flags);
  pf("# zCmppD=%s\n# zBaseName=%s\n",
     pB->zCmppD ? pB->zCmppD : "", zBaseName);

  pf("logtag.%s ?= [%s %s $@]:\n", zBuildName, pB->zEmo, zBuildName);
  if( pB->zIfCond ){
    pf("%s\n", pB->zIfCond );
  }
  pf("$(info $(logtag.%s) Setting up target b-%s)\n",
     zBuildName, zBuildName );

  emit_core_vars(zBuildName, pB);
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

  { /* Create sqlite3-api.*.js */
    pf("sqlite3-api.%s.c-pp.js = $(dir.tmp)/sqlite3-api.%s.c-pp%s\n",
       zBuildName, zBuildName, zJsExt);

    pf("sqlite3-api.%s.js = $(dir.tmp)/sqlite3-api.%s%s\n",
       zBuildName, zBuildName, zJsExt);
    pf("c-pp.D.%s = %s\n", zBuildName, pB->zCmppD ? pB->zCmppD: "" );

    pf("$(sqlite3-api.%s.c-pp.js): $(sqlite3-api.jses)\n"
       "\t@echo '$(logtag.%s) $(emo.disk)'; \\\n"
       "\tmkdir -p $(dir.dout.%s); \\\n"
       "\tfor i in $(sqlite3-api.jses); do \\\n"
       "\t\techo \"/* BEGIN FILE: $$i */\"; \\\n"
       "\t\tcat $$i; \\\n"
       "\t\techo \"/* END FILE: $$i */\"; \\\n"
       "\tdone > $@\n",
       zBuildName, zBuildName, zBuildName);

    pf("$(sqlite3-api.%s.js): $(sqlite3-api.%s.c-pp.js)\n"
       "$(eval $(call b.do.c-pp,"
       "%s,"
       "$(sqlite3-api.%s.c-pp.js),"
       "$(sqlite3-api.%s.js),"
       "$(c-pp.D.%s)"
       "))\n",
       zBuildName, zBuildName,
       zBuildName, zBuildName,
       zBuildName, zBuildName);
  }

  mk_pre_post(zBuildName);

  { /* build it... */
    pf(zBanner
       "# Build $(out.%s.js)\n"
       "$(out.%s.js): $(MAKEFILE_LIST) $(sqlite3-wasm.cfiles) $(EXPORTED_FUNCTIONS.api)"
       " $(bin.mkwb) $(pre-post.%s.deps) $(sqlite3-api.ext.jses)"
       /* maintenance reminder: we set these ^^^^ as deps so that they
       ** get copied into place early. That allows the developer to
       ** reload the base-most test pages while the later-stage builds
       ** are still compiling, which is especially helpful when
       ** running builds with long build times (like -Oz). */
       "\n",
       zBuildName, zBuildName, zBuildName);

    pf("\t@echo '$(logtag.%s) $(emo.compile) building ...'\n"
       "\t@mkdir -p $(dir $@);\n",
       zBuildName);

    if( F_UNSUPPORTED & pB->flags ){
      pf("\t@echo '$(logtag.%s) $(emo.fire)$(emo.fire)$(emo.fire): "
         "unsupported build. Use at your own risk.'\n", zBuildName);
    }
    pf("\t$(cmd.loud)$(call b.do.emcc,%s)\n", zBuildName);

    { /* Post-compilation transformations and copying to
         $(dir.dout)... */
      if( (F_ESM & pB->flags) || (F_NODEJS & pB->flags) ){
        /* TODO? Replace this $(call) with the corresponding makefile
        ** code.  OTOH, we also use this $(call) in the speedtest1-wasmfs
        ** build, which is not part of the rules emitted by this
        ** program. */
        pf("\t@$(call SQLITE.CALL.xJS.ESM-EXPORT-DEFAULT,1,%d,$(logtag.%s))\n",
           (F_WASMFS & pB->flags) ? 1 : 0,
           zBuildName
        );
      }

      pf("\t@chmod -x $(out.%s.wasm)\n", zBuildName
         /* althttpd will automatically try to execute wasm files
            if they have the +x bit set. Why that bit is set
            at all is a mystery. */);
      pf("\t@$(call b.do.wasm-strip,%s)\n", zBuildName);

      pf("\t@$(call SQLITE.CALL.WASM-OPT,%s,$(logtag.%s))\n",
         zBuildName, zBuildName);
      pf("\t@$(call SQLITE.strip-emcc-js-cruft,$(logtag.%s))\n", zBuildName);

      if( CP_JS & pB->flags && !(pB->zDotWasm/*handled below*/) ){
        pf("\t@cp -f $@ $(dir.dout)/. || exit; \\\n"
           "\techo '$(logtag.%s) $(emo.disk) $(dir.dout)/$(notdir $@)'\n",
           zBuildName
        );
      }
      if( CP_WASM & pB->flags ){
        pf("\t@cp -f $(out.%s.wasm) $(dir.dout)/. || exit; \\\n"
           "\techo '[%s %s $(out.%s.wasm)] $(emo.disk) "
           "$(dir.dout)/$(notdir $(out.%s.wasm))'\n",
           zBuildName, pB->zEmo, zBuildName, zBuildName, zBuildName);
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
  pf("\t@echo '$(logtag.%s) $(emo.done)'\n", zBuildName);

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

  if( 0==(F_NOT_IN_ALL & pB->flags)
      && 0==(F_UNSUPPORTED & pB->flags) ){
    pf("all: $(out.%s.js)\n", zBuildName);
  }

  if( pB->zIfCond ){
    pf("else\n"
       "$(info $(logtag.%s) $(emo.stop) disabled by condition: %s)\n"
       "endif\n",
       zBuildName, pB->zIfCond);
  }
  pf("# End build [%s]%s", zBuildName, zBanner);
}

/*
** Emits rules for the fiddle builds.
*/
void mk_fiddle(void){
  //BuildDef bdF = oBuildDefs.vanilla;

  //mk_pre_post("fiddle-module");
  //mk_pre_post("fiddle-module.debug");
  pf("emo.fiddle = 🎻\n");
  for(int i = 0; i < 2; ++i ){
    /* 0==normal, 1==debug */
    int const isDebug = i>0;
    const char * const zBuildName = i ? "fiddle.debug" : "fiddle";
    //const char *zDir = i ? "$(dir.fiddle-debug)" : "$(dir.fiddle)";

    pf("%s# Begin %s\n", zBanner, zBuildName);
    if( isDebug ){
      pf("emo.%s = $(emo.fiddle)$(emo.bug)\n", zBuildName);
    }
    pf("logtag.%s ?= [$(emo.%s) %s $@]:\n",
       zBuildName, zBuildName, zBuildName);
    pf("$(info $(logtag.%s) Setting up target %s)\n",
       zBuildName, zBuildName );

    //bdF.zBaseName = "fiddle-module";
    //bdF.zEmcc = i ? "$(fiddle.emcc-flags.debug)" : "$(fiddle.emcc-flags)";

    pf("dir.%s = %s\n"
       "out.%s.js = $(dir.%s)/fiddle-module.js\n"
       "out.%s.wasm = $(dir.%s)/fiddle-module.wasm\n"
       "$(out.%s.wasm): $(out.%s.js)\n",
       zBuildName, zBuildName,
       zBuildName, zBuildName,
       zBuildName, zBuildName,
       zBuildName, zBuildName);

    {/* emcc */
      pf("$(out.%s.js): $(MAKEFILE_LIST) $(MAKEFILE.fiddle) "
         "$(EXPORTED_FUNCTIONS.fiddle) "
         "$(fiddle.cses) %s "
         "$(SOAP.js)\n",
         zBuildName,
         (i ? " $(out.fiddle.js)" : ""));
      pf("\t@echo '$(logtag.%s) $(emo.compile) building ...'\n",
         zBuildName);
      if( isDebug ){
        pf("\t@test -d \"$(dir $@)\" || mkdir -p \"$(dir $@)\"\n");
      }
      pf("\t$(bin.emcc) -o $@ $(emcc.flags.%s) "
         /* emcc.flags.X is set in fiddle.make */
         //"$(pre-post-fiddle-module.flags) "
         "$(fiddle.cses)\n",
         zBuildName);
      pf("\t@chmod -x $(out.%s.wasm)\n", zBuildName);
      pf("\t@$(call b.do.wasm-strip,%s)\n", zBuildName);
      pf("\t@$(call SQLITE.strip-emcc-js-cruft,$(logtag.%s))\n",zBuildName);
      pf("\t@cp -p $(SOAP.js) $(dir $@)\n");
      if( isDebug ){
        ps("\tcp -p $(dir.fiddle)/index.html "
           "$(dir.fiddle)/fiddle.js "
           "$(dir.fiddle)/fiddle-worker.js "
           "$(dir $@)");
      }
    }
    if( !isDebug ){
      pf("all: $(out.%s.wasm)\n", zBuildName);
    }

    /* Compress fiddle files. We handle each file separately, rather
       than compressing them in a loop in the previous target, to help
       avoid that hand-edited files, like fiddle-worker.js, do not end
       up with stale .gz files (which althttpd will then serve instead
       of the up-to-date uncompressed one). */
    pf("\n$(out.%s.js).gz: $(out.%s.js)\n"
       "\tgzip < $< > $@\n", zBuildName, zBuildName);
    pf("\n$(out.%s.wasm).gz: $(out.%s.wasm)\n"
       "\tgzip < $< > $@\n", zBuildName, zBuildName);

    pf("\n%s: $(out.%s.js).gz $(out.%s.wasm).gz\n",
       zBuildName, zBuildName, zBuildName);
    if( isDebug ){
      ps("fiddle-debug: fiddle.debug");
    }
    pf("# End %s%s", zBuildName, zBanner);
  }
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
#if 0
  mk_pre_post(0, "speedtest1","vanilla", 0, "speedtest1.wasm");
  mk_pre_post(0, "speedtest1-wasmfs", "esm",
              "$(c-pp.D.sqlite3-bundler-friendly) -Dwasmfs",
              "speetest1-wasmfs.wasm");
#endif
  return rc;
}
