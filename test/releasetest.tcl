#!/usr/bin/tclsh
#
# Documentation for this script. This may be output to stderr
# if the script is invoked incorrectly. See the [process_options]
# proc below.
#
set ::USAGE_MESSAGE {
This Tcl script is used to test the various configurations required
before releasing a new version. Supported command line options (all
optional) are:

    --srcdir   TOP-OF-SQLITE-TREE      (see below)
    --platform PLATFORM                (see below)
    --config   CONFIGNAME              (Run only CONFIGNAME)
    --quick                            (Run "veryquick.test" only)
    --veryquick                        (Run "make smoketest" only)
    --msvc                             (Use MSVC as the compiler)
    --buildonly                        (Just build testfixture - do not run)
    --dryrun                           (Print what would have happened)
    --info                             (Show diagnostic info)
    --with-tcl=DIR                     (Use TCL build at DIR)

The default value for --srcdir is the parent of the directory holding
this script.

The script determines the default value for --platform using the
$tcl_platform(os) and $tcl_platform(machine) variables.  Supported
platforms are "Linux-x86", "Linux-x86_64", "Darwin-i386",
"Darwin-x86_64", "Windows NT-intel", and "Windows NT-amd64".

Every test begins with a fresh run of the configure script at the top
of the SQLite source tree.
}

# Omit comments (text between # and \n) in a long multi-line string.
#
proc strip_comments {in} {
  regsub -all {#[^\n]*\n} $in {} out
  return $out
}

array set ::Configs [strip_comments {
  "Default" {
    -O2
    --disable-amalgamation --disable-shared
  }
  "Sanitize" {
    CC=clang -fsanitize=undefined
    -DSQLITE_ENABLE_STAT4
  }
  "Have-Not" {
    # The "Have-Not" configuration sets all possible -UHAVE_feature options
    # in order to verify that the code works even on platforms that lack
    # these support services.
    -DHAVE_FDATASYNC=0
    -DHAVE_GMTIME_R=0
    -DHAVE_ISNAN=0
    -DHAVE_LOCALTIME_R=0
    -DHAVE_LOCALTIME_S=0
    -DHAVE_MALLOC_USABLE_SIZE=0
    -DHAVE_STRCHRNUL=0
    -DHAVE_USLEEP=0
    -DHAVE_UTIME=0
  }
  "Unlock-Notify" {
    -O2
    -DSQLITE_ENABLE_UNLOCK_NOTIFY
    -DSQLITE_THREADSAFE
    -DSQLITE_TCL_DEFAULT_FULLMUTEX=1
  }
  "Secure-Delete" {
    -O2
    -DSQLITE_SECURE_DELETE=1
    -DSQLITE_SOUNDEX=1
  }
  "Update-Delete-Limit" {
    -O2
    -DSQLITE_DEFAULT_FILE_FORMAT=4
    -DSQLITE_ENABLE_UPDATE_DELETE_LIMIT=1
    -DSQLITE_ENABLE_STMT_SCANSTATUS
    --enable-json1
  }
  "Check-Symbols" {
    -DSQLITE_MEMDEBUG=1
    -DSQLITE_ENABLE_FTS3_PARENTHESIS=1
    -DSQLITE_ENABLE_FTS3=1
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_ENABLE_MEMSYS5=1
    -DSQLITE_ENABLE_MEMSYS3=1
    -DSQLITE_ENABLE_COLUMN_METADATA=1
    -DSQLITE_ENABLE_UPDATE_DELETE_LIMIT=1
    -DSQLITE_SECURE_DELETE=1
    -DSQLITE_SOUNDEX=1
    -DSQLITE_ENABLE_ATOMIC_WRITE=1
    -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1
    -DSQLITE_ENABLE_OVERSIZE_CELL_CHECK=1
    -DSQLITE_ENABLE_STAT4
    -DSQLITE_ENABLE_STMT_SCANSTATUS
    --enable-json1 --enable-fts5
  }
  "Debug-One" {
    --disable-shared
    -O2
    -DSQLITE_DEBUG=1
    -DSQLITE_MEMDEBUG=1
    -DSQLITE_MUTEX_NOOP=1
    -DSQLITE_TCL_DEFAULT_FULLMUTEX=1
    -DSQLITE_ENABLE_FTS3=1
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_ENABLE_MEMSYS5=1
    -DSQLITE_ENABLE_MEMSYS3=1
    -DSQLITE_ENABLE_COLUMN_METADATA=1
    -DSQLITE_ENABLE_STAT4
    -DSQLITE_MAX_ATTACHED=125
  }
  "Fast-One" {
    -O6
    -DSQLITE_ENABLE_FTS4=1
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_ENABLE_STAT4
    -DSQLITE_ENABLE_RBU
    -DSQLITE_MAX_ATTACHED=125
  }
  "Device-One" {
    -O2
    -DSQLITE_DEBUG=1
    -DSQLITE_DEFAULT_AUTOVACUUM=1
    -DSQLITE_DEFAULT_CACHE_SIZE=64
    -DSQLITE_DEFAULT_PAGE_SIZE=1024
    -DSQLITE_DEFAULT_TEMP_CACHE_SIZE=32
    -DSQLITE_DISABLE_LFS=1
    -DSQLITE_ENABLE_ATOMIC_WRITE=1
    -DSQLITE_ENABLE_IOTRACE=1
    -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1
    -DSQLITE_MAX_PAGE_SIZE=4096
    -DSQLITE_OMIT_LOAD_EXTENSION=1
    -DSQLITE_OMIT_PROGRESS_CALLBACK=1
    -DSQLITE_OMIT_VIRTUALTABLE=1
    -DSQLITE_TEMP_STORE=3
    --enable-json1
  }
  "Device-Two" {
    -DSQLITE_4_BYTE_ALIGNED_MALLOC=1
    -DSQLITE_DEFAULT_AUTOVACUUM=1
    -DSQLITE_DEFAULT_CACHE_SIZE=1000
    -DSQLITE_DEFAULT_LOCKING_MODE=0
    -DSQLITE_DEFAULT_PAGE_SIZE=1024
    -DSQLITE_DEFAULT_TEMP_CACHE_SIZE=1000
    -DSQLITE_DISABLE_LFS=1
    -DSQLITE_ENABLE_FTS3=1
    -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_MAX_COMPOUND_SELECT=50
    -DSQLITE_MAX_PAGE_SIZE=32768
    -DSQLITE_OMIT_TRACE=1
    -DSQLITE_TEMP_STORE=3
    -DSQLITE_THREADSAFE=2
    --enable-json1 --enable-fts5
  }
  "Locking-Style" {
    -O2
    -DSQLITE_ENABLE_LOCKING_STYLE=1
  }
  "OS-X" {
    -O1   # Avoid a compiler bug in gcc 4.2.1 build 5658
    -DSQLITE_OMIT_LOAD_EXTENSION=1
    -DSQLITE_DEFAULT_MEMSTATUS=0
    -DSQLITE_THREADSAFE=2
    -DSQLITE_OS_UNIX=1
    -DSQLITE_ENABLE_JSON1=1
    -DSQLITE_ENABLE_LOCKING_STYLE=1
    -DUSE_PREAD=1
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_ENABLE_FTS3=1
    -DSQLITE_ENABLE_FTS3_PARENTHESIS=1
    -DSQLITE_DEFAULT_CACHE_SIZE=1000
    -DSQLITE_MAX_LENGTH=2147483645
    -DSQLITE_MAX_VARIABLE_NUMBER=500000
    -DSQLITE_DEBUG=1
    -DSQLITE_PREFER_PROXY_LOCKING=1
    -DSQLITE_ENABLE_API_ARMOR=1
    --enable-json1 --enable-fts5
  }
  "Extra-Robustness" {
    -DSQLITE_ENABLE_OVERSIZE_CELL_CHECK=1
    -DSQLITE_MAX_ATTACHED=62
  }
  "Devkit" {
    -DSQLITE_DEFAULT_FILE_FORMAT=4
    -DSQLITE_MAX_ATTACHED=30
    -DSQLITE_ENABLE_COLUMN_METADATA
    -DSQLITE_ENABLE_FTS4
    -DSQLITE_ENABLE_FTS4_PARENTHESIS
    -DSQLITE_DISABLE_FTS4_DEFERRED
    -DSQLITE_ENABLE_RTREE
    --enable-json1 --enable-fts5
  }
  "No-lookaside" {
    -DSQLITE_TEST_REALLOC_STRESS=1
    -DSQLITE_OMIT_LOOKASIDE=1
    -DHAVE_USLEEP=1
  }
  "Valgrind" {
    -DSQLITE_ENABLE_STAT4
    -DSQLITE_ENABLE_FTS4
    -DSQLITE_ENABLE_RTREE
    --enable-json1
  }

  # The next group of configurations are used only by the
  # Failure-Detection platform.  They are all the same, but we need
  # different names for them all so that they results appear in separate
  # subdirectories.
  #
  Fail0 {-O0}
  Fail2 {-O0}
  Fail3 {-O0}
  Fail4 {-O0}
  FuzzFail1 {-O0}
  FuzzFail2 {-O0}
}]

array set ::Platforms [strip_comments {
  Linux-x86_64 {
    "Check-Symbols"           checksymbols
    "Debug-One"               "mptest test"
    "Have-Not"                test
    "Secure-Delete"           test
    "Unlock-Notify"           "QUICKTEST_INCLUDE=notify2.test test"
    "Update-Delete-Limit"     test
    "Extra-Robustness"        test
    "Device-Two"              test
    "No-lookaside"            test
    "Devkit"                  test
    "Sanitize"                {QUICKTEST_OMIT=func4.test,nan.test test}
    "Fast-One"                fuzztest
    "Valgrind"                valgrindtest
    "Default"                 "threadtest fulltest"
    "Device-One"              fulltest
  }
  Linux-i686 {
    "Devkit"                  test
    "Have-Not"                test
    "Unlock-Notify"           "QUICKTEST_INCLUDE=notify2.test test"
    "Device-One"              test
    "Device-Two"              test
    "Default"                 "threadtest fulltest"
  }
  Darwin-i386 {
    "Locking-Style"           "mptest test"
    "Have-Not"                test
    "OS-X"                    "threadtest fulltest"
  }
  Darwin-x86_64 {
    "Locking-Style"           "mptest test"
    "Have-Not"                test
    "OS-X"                    "threadtest fulltest"
  }
  "Windows NT-intel" {
    "Default"                 "mptest fulltestonly"
    "Have-Not"                test
  }
  "Windows NT-amd64" {
    "Default"                 "mptest fulltestonly"
    "Have-Not"                test
  }

  # The Failure-Detection platform runs various tests that deliberately
  # fail.  This is used as a test of this script to verify that this script
  # correctly identifies failures.
  #
  Failure-Detection {
    Fail0     "TEST_FAILURE=0 test"
    Sanitize  "TEST_FAILURE=1 test"
    Fail2     "TEST_FAILURE=2 valgrindtest"
    Fail3     "TEST_FAILURE=3 valgrindtest"
    Fail4     "TEST_FAILURE=4 test"
    FuzzFail1 "TEST_FAILURE=5 test"
    FuzzFail2 "TEST_FAILURE=5 valgrindtest"
  }
}]


# End of configuration section.
#########################################################################
#########################################################################

foreach {key value} [array get ::Platforms] {
  foreach {v t} $value {
    if {0==[info exists ::Configs($v)]} {
      puts stderr "No such configuration: \"$v\""
      exit -1
    }
  }
}

# Output log
#
set LOG [open releasetest-out.txt w]
proc PUTS {args} {
  if {[llength $args]==2} {
    puts [lindex $args 0] [lindex $args 1]
    puts $::LOG [lindex $args 1]
  } else {
    puts [lindex $args 0]
    puts $::LOG [lindex $args 0]
  }
  flush $::LOG
}
puts $LOG "$argv0 $argv"
set tm0 [clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S} -gmt 1]
puts $LOG "start-time: $tm0 UTC"

# Open the file $logfile and look for a report on the number of errors
# and the number of test cases run.  Add these values to the global
# $::NERRCASE and $::NTESTCASE variables.
#
# If any errors occur, then write into $errmsgVar the text of an appropriate
# one-line error message to show on the output.
#
proc count_tests_and_errors {logfile rcVar errmsgVar} {
  if {$::DRYRUN} return
  upvar 1 $rcVar rc $errmsgVar errmsg
  set fd [open $logfile rb]
  set seen 0
  while {![eof $fd]} {
    set line [gets $fd]
    if {[regexp {(\d+) errors out of (\d+) tests} $line all nerr ntest]} {
      incr ::NERRCASE $nerr
      incr ::NTESTCASE $ntest
      set seen 1
      if {$nerr>0} {
        set rc 1
        set errmsg $line
      }
    }
    if {[regexp {runtime error: +(.*)} $line all msg]} {
      # skip over "value is outside range" errors
      if {[regexp {value .* is outside the range of representable} $line]} {
         # noop
      } else {
        incr ::NERRCASE
        if {$rc==0} {
          set rc 1
          set errmsg $msg
        }
      }
    }
    if {[regexp {fatal error +(.*)} $line all msg]} {
      incr ::NERRCASE
      if {$rc==0} {
        set rc 1
        set errmsg $msg
      }
    }
    if {[regexp {ERROR SUMMARY: (\d+) errors.*} $line all cnt] && $cnt>0} {
      incr ::NERRCASE
      if {$rc==0} {
        set rc 1
        set errmsg $all
      }
    }
    if {[regexp {^VERSION: 3\.\d+.\d+} $line]} {
      set v [string range $line 9 end]
      if {$::SQLITE_VERSION eq ""} {
        set ::SQLITE_VERSION $v
      } elseif {$::SQLITE_VERSION ne $v} {
        set rc 1
        set errmsg "version conflict: {$::SQLITE_VERSION} vs. {$v}"
      }
    }
  }
  close $fd
  if {$::BUILDONLY} {
    if {$rc==0} {
      set errmsg "Build complete"
    } else {
      set errmsg "Build failed"
    }
  } elseif {!$seen} {
    set rc 1
    set errmsg "Test did not complete"
    if {[file readable core]} {
      append errmsg " - core file exists"
    }
  }
}

proc run_test_suite {name testtarget config} {
  # Tcl variable $opts is used to build up the value used to set the
  # OPTS Makefile variable. Variable $cflags holds the value for
  # CFLAGS. The makefile will pass OPTS to both gcc and lemon, but
  # CFLAGS is only passed to gcc.
  #
  set cflags [expr {$::MSVC ? "-Zi" : "-g"}]
  set opts ""
  set title ${name}($testtarget)
  set configOpts $::WITHTCL

  regsub -all {#[^\n]*\n} $config \n config
  foreach arg $config {
    if {[regexp {^-[UD]} $arg]} {
      lappend opts $arg
    } elseif {[regexp {^[A-Z]+=} $arg]} {
      lappend testtarget $arg
    } elseif {[regexp {^--(enable|disable)-} $arg]} {
      lappend configOpts $arg
    } else {
      lappend cflags $arg
    }
  }

  set cflags [join $cflags " "]
  set opts   [join $opts " "]
  append opts " -DSQLITE_NO_SYNC=1"

  # Some configurations already set HAVE_USLEEP; in that case, skip it.
  #
  if {![regexp { -DHAVE_USLEEP$} $opts]
         && ![regexp { -DHAVE_USLEEP[ =]+} $opts]} {
    append opts " -DHAVE_USLEEP=1"
  }

  # Set the sub-directory to use.
  #
  set dir [string tolower [string map {- _ " " _} $name]]

  if {$::tcl_platform(platform)=="windows"} {
    append opts " -DSQLITE_OS_WIN=1"
  } else {
    append opts " -DSQLITE_OS_UNIX=1"
  }

  if {!$::TRACE} {
    set n [string length $title]
    PUTS -nonewline "${title}[string repeat . [expr {63-$n}]]"
    flush stdout
  }

  set rc 0
  set tm1 [clock seconds]
  set origdir [pwd]
  trace_cmd file mkdir $dir
  trace_cmd cd $dir
  set errmsg {}
  catch {file delete core}
  set rc [catch [configureCommand $configOpts]]
  if {!$rc} {
    set rc [catch [makeCommand $testtarget $cflags $opts]]
    count_tests_and_errors test.log rc errmsg
  }
  trace_cmd cd $origdir
  set tm2 [clock seconds]

  if {!$::TRACE} {
    set hours [expr {($tm2-$tm1)/3600}]
    set minutes [expr {(($tm2-$tm1)/60)%60}]
    set seconds [expr {($tm2-$tm1)%60}]
    set tm [format (%02d:%02d:%02d) $hours $minutes $seconds]
    if {$rc} {
      PUTS " FAIL $tm"
      incr ::NERR
    } else {
      PUTS " Ok   $tm"
    }
    if {$errmsg!=""} {PUTS "     $errmsg"}
  }
}

# The following procedure returns the "configure" command to be exectued for
# the current platform, which may be Windows (via MinGW, etc).
#
proc configureCommand {opts} {
  if {$::MSVC} return [list]; # This is not needed for MSVC.
  set result [list trace_cmd exec]
  if {$::tcl_platform(platform)=="windows"} {
    lappend result sh
  }
  lappend result $::SRCDIR/configure --enable-load-extension
  foreach x $opts {lappend result $x}
  lappend result >& test.log
}

# The following procedure returns the "make" command to be executed for the
# specified targets, compiler flags, and options.
#
proc makeCommand { targets cflags opts } {
  set result [list trace_cmd exec]
  if {$::MSVC} {
    set nmakeDir [file nativename $::SRCDIR]
    set nmakeFile [file join $nmakeDir Makefile.msc]
    lappend result nmake /f $nmakeFile TOP=$nmakeDir clean
  } else {
    lappend result make clean
  }
  foreach target $targets {
    lappend result $target
  }
  lappend result CFLAGS=$cflags OPTS=$opts >>& test.log
}

# The following procedure prints its arguments if ::TRACE is true.
# And it executes the command of its arguments in the calling context
# if ::DRYRUN is false.
#
proc trace_cmd {args} {
  if {$::TRACE} {
    PUTS $args
  }
  if {!$::DRYRUN} {
    uplevel 1 $args
  }
}


# This proc processes the command line options passed to this script.
# Currently the only option supported is "-makefile", default
# "releasetest.mk". Set the ::MAKEFILE variable to the value of this
# option.
#
proc process_options {argv} {
  set ::SRCDIR    [file normalize [file dirname [file dirname $::argv0]]]
  set ::QUICK     0
  set ::MSVC      0
  set ::BUILDONLY 0
  set ::DRYRUN    0
  set ::EXEC      exec
  set ::TRACE     0
  set ::WITHTCL   {}
  set config {}
  set platform $::tcl_platform(os)-$::tcl_platform(machine)

  for {set i 0} {$i < [llength $argv]} {incr i} {
    set x [lindex $argv $i]
    if {[regexp {^--[a-z]} $x]} {set x [string range $x 1 end]}
    switch -glob -- $x {
      -srcdir {
        incr i
        set ::SRCDIR [file normalize [lindex $argv $i]]
      }

      -platform {
        incr i
        set platform [lindex $argv $i]
      }

      -quick {
        set ::QUICK 1
      }
      -veryquick {
        set ::QUICK 2
      }

      -config {
        incr i
        set config [lindex $argv $i]
      }

      -msvc {
        set ::MSVC 1
      }

      -buildonly {
        set ::BUILDONLY 1
      }

      -dryrun {
        set ::DRYRUN 1
      }

      -trace {
        set ::TRACE 1
      }

      -info {
        PUTS "Command-line Options:"
        PUTS "   --srcdir $::SRCDIR"
        PUTS "   --platform [list $platform]"
        PUTS "   --config [list $config]"
        if {$::QUICK} {
          if {$::QUICK==1} {PUTS "   --quick"}
          if {$::QUICK==2} {PUTS "   --veryquick"}
        }
        if {$::MSVC}      {PUTS "   --msvc"}
        if {$::BUILDONLY} {PUTS "   --buildonly"}
        if {$::DRYRUN}    {PUTS "   --dryrun"}
        if {$::TRACE}     {PUTS "   --trace"}
        PUTS "\nAvailable --platform options:"
        foreach y [lsort [array names ::Platforms]] {
          PUTS "   [list $y]"
        }
        PUTS "\nAvailable --config options:"
        foreach y [lsort [array names ::Configs]] {
          PUTS "   [list $y]"
        }
        exit
      }

      -g {
        if {$::MSVC} {
          lappend ::EXTRACONFIG -Zi
        } else {
          lappend ::EXTRACONFIG [lindex $argv $i]
        }
      }

      -with-tcl=* {
        set ::WITHTCL -$x
      }

      -D* -
      -O* -
      -enable-* -
      -disable-* -
      *=* {
        lappend ::EXTRACONFIG [lindex $argv $i]
      }

      default {
        PUTS stderr ""
        PUTS stderr [string trim $::USAGE_MESSAGE]
        exit -1
      }
    }
  }

  if {0==[info exists ::Platforms($platform)]} {
    PUTS "Unknown platform: $platform"
    PUTS -nonewline "Set the -platform option to "
    set print [list]
    foreach p [array names ::Platforms] {
      lappend print "\"$p\""
    }
    lset print end "or [lindex $print end]"
    PUTS "[join $print {, }]."
    exit
  }

  if {$config!=""} {
    if {[llength $config]==1} {lappend config fulltest}
    set ::CONFIGLIST $config
  } else {
    set ::CONFIGLIST $::Platforms($platform)
  }
  PUTS "Running the following test configurations for $platform:"
  PUTS "    [string trim $::CONFIGLIST]"
  PUTS -nonewline "Flags:"
  if {$::DRYRUN} {PUTS -nonewline " --dryrun"}
  if {$::BUILDONLY} {PUTS -nonewline " --buildonly"}
  if {$::MSVC} {PUTS -nonewline " --msvc"}
  switch -- $::QUICK {
     1 {PUTS -nonewline " --quick"}
     2 {PUTS -nonewline " --veryquick"}
  }
  PUTS ""
}

# Main routine.
#
proc main {argv} {

  # Process any command line options.
  set ::EXTRACONFIG {}
  process_options $argv
  PUTS [string repeat * 79]

  set ::NERR 0
  set ::NTEST 0
  set ::NTESTCASE 0
  set ::NERRCASE 0
  set ::SQLITE_VERSION {}
  set STARTTIME [clock seconds]
  foreach {zConfig target} $::CONFIGLIST {
    if {$::MSVC && ($zConfig eq "Sanitize" || "checksymbols" in $target
           || "valgrindtest" in $target)} {
      PUTS "Skipping $zConfig / $target for MSVC..."
      continue
    }
    if {$target ne "checksymbols"} {
      switch -- $::QUICK {
         1 {set target quicktest}
         2 {set target smoketest}
      }
      if {$::BUILDONLY} {
        set target testfixture
        if {$::MSVC} {append target .exe}
      }
    }
    set config_options [concat $::Configs($zConfig) $::EXTRACONFIG]

    incr NTEST
    run_test_suite $zConfig $target $config_options

    # If the configuration included the SQLITE_DEBUG option, then remove
    # it and run veryquick.test. If it did not include the SQLITE_DEBUG option
    # add it and run veryquick.test.
    if {$target!="checksymbols" && $target!="valgrindtest"
           && $target!="fuzzoomtest" && !$::BUILDONLY && $::QUICK<2} {
      set debug_idx [lsearch -glob $config_options -DSQLITE_DEBUG*]
      set xtarget $target
      regsub -all {fulltest[a-z]*} $xtarget test xtarget
      regsub -all {fuzzoomtest} $xtarget fuzztest xtarget
      if {$debug_idx < 0} {
        incr NTEST
        append config_options " -DSQLITE_DEBUG=1"
        run_test_suite "${zConfig}_debug" $xtarget $config_options
      } else {
        incr NTEST
        regsub { *-DSQLITE_MEMDEBUG[^ ]* *} $config_options { } config_options
        regsub { *-DSQLITE_DEBUG[^ ]* *} $config_options { } config_options
        run_test_suite "${zConfig}_ndebug" $xtarget $config_options
      }
    }
  }

  set elapsetime [expr {[clock seconds]-$STARTTIME}]
  set hr [expr {$elapsetime/3600}]
  set min [expr {($elapsetime/60)%60}]
  set sec [expr {$elapsetime%60}]
  set etime [format (%02d:%02d:%02d) $hr $min $sec]
  PUTS [string repeat * 79]
  incr ::NERRCASE $::NERR
  PUTS "$::NERRCASE failures out of $::NTESTCASE tests in $etime"
  if {$::SQLITE_VERSION ne ""} {
    PUTS "SQLite $::SQLITE_VERSION"
  }
}

main $argv
