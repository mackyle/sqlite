#!/usr/bin/tclsh
#
# This script constructs the "sqlite3.h" header file from the following
# sources:
#
#   1) The src/sqlite.h.in source file.  This is the template for sqlite3.h.
#   2) The VERSION file containing the current SQLite version number.
#   3) The manifest file from the fossil SCM.  This gives use the date.
#   4) The manifest.uuid file from the fossil SCM.  This gives the SHA1 hash.
#
# Run this script by specifying the root directory of the source tree
# on the command-line.
#
# This script performs processing on src/sqlite.h.in. It:
#
#   1) Adds SQLITE_EXTERN in front of the declaration of global variables,
#   2) Adds SQLITE_API in front of the declaration of API functions,
#   3) Replaces the string --VERS-- with the current library version,
#      formatted as a string (e.g. "3.6.17"), and
#   4) Replaces the string --VERSION-NUMBER-- with current library version,
#      formatted as an integer (e.g. "3006017").
#   5) Replaces the string --SOURCE-ID-- with the date and time and sha1
#      hash of the fossil-scm manifest for the source tree.
#   6) Adds the SQLITE_CALLBACK calling convention macro in front of all
#      callback declarations.
#
# This script outputs to stdout unless the -o FILENAME option is used.
#
# Example usage:
#
#   tclsh mksqlite3h.tcl ../sqlite [OPTIONS]
#                        ^^^^^^^^^
#                        Root of source tree
#
# Where options are:
#
#   --enable-recover          Include the sqlite3recover extension
#   -o FILENAME               Write results to FILENAME instead of stdout
#   --useapicall              SQLITE_APICALL instead of SQLITE_CDECL
#

# Default output stream
set out stdout

# Get the source tree root directory from the command-line
#
set TOP [lindex $argv 0]

# If the -o FILENAME option is present, use FILENAME for output.
#
set x [lsearch $argv -o]
if {$x>0} {
  incr x
  set out [open [lindex $argv $x] wb]
}

# Enable use of SQLITE_APICALL macros at the right points?
#
set useapicall 0

# Include sqlite3recover.h?
#
set enable_recover 0

# Process command-line arguments
if {[lsearch -regexp [lrange $argv 1 end] {^-+useapicall}] != -1} {
  set useapicall 1
}
if {[lsearch -regexp [lrange $argv 1 end] {^-+enable-recover}] != -1} {
  set enable_recover 1
}

# Get the SQLite version number (ex: 3.6.18) from the $TOP/VERSION file.
#
set in [open [file normalize $TOP/VERSION] rb]
set zVersion [string trim [read $in]]
close $in
set nVersion [eval format "%d%03d%03d" [split $zVersion .]]

# Get the source-id
#
set PWD [pwd]
cd $TOP
set tmpfile $PWD/tmp-[clock millisec]-[expr {int(rand()*100000000000)}].txt
set mksourceid $PWD/mksourceid
if {![file exists $mksourceid] && [file exists ${mksourceid}.exe]} {
  # Workaround for Windows-based Unix-like environments
  # https://sqlite.org/forum/forumpost/41ba710dd9943453
  set mksourceid ${mksourceid}.exe
}
exec $mksourceid manifest > $tmpfile
set fd [open $tmpfile rb]
set zSourceId [string trim [read $fd]]
close $fd
file delete -force $tmpfile
cd $PWD

# Set up patterns for recognizing API declarations.
#
set varpattern {^[a-zA-Z][a-zA-Z_0-9 *]+sqlite3_[_a-zA-Z0-9]+(\[|;| =)}
set declpattern1 {^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(sqlite3_[_a-zA-Z0-9]+)(\(.*)$}

set declpattern2 \
    {^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(sqlite3session_[_a-zA-Z0-9]+)(\(.*)$}

set declpattern3 \
    {^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(sqlite3changeset_[_a-zA-Z0-9]+)(\(.*)$}

set declpattern4 \
    {^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(sqlite3changegroup_[_a-zA-Z0-9]+)(\(.*)$}

set declpattern5 \
    {^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(sqlite3rebaser_[_a-zA-Z0-9]+)(\(.*)$}

# Force the output to use unix line endings, even on Windows.
fconfigure stdout -translation binary

set filelist [subst {
  $TOP/src/sqlite.h.in
  $TOP/ext/rtree/sqlite3rtree.h
  $TOP/ext/session/sqlite3session.h
  $TOP/ext/fts5/fts5.h
}]
if {$enable_recover} {
  lappend filelist "$TOP/ext/recover/sqlite3recover.h"
}

# These are the functions that accept a variable number of arguments.  They
# always need to use the "cdecl" calling convention even when another calling
# convention (e.g. "stcall") is being used for the rest of the library.
set cdecllist {
  sqlite3_config
  sqlite3_db_config
  sqlite3_log
  sqlite3_mprintf
  sqlite3_snprintf
  sqlite3_test_control
  sqlite3_vtab_config
}

# Process the source files.
#
foreach file $filelist {
  set in [open $file rb]
  if {![regexp {sqlite\.h\.in} $file]} {
    puts $out "/******** Begin file [file tail $file] *********/"
  }
  while {![eof $in]} {

    set line [string trimright [gets $in]]

    # File sqlite3rtree.h contains a line "#include <sqlite3.h>". Omit this
    # line when copying sqlite3rtree.h into sqlite3.h.
    #
    if {[string match {*#include*[<"]sqlite3.h[>"]*} $line]} continue

    regsub -- --VERS--           $line $zVersion line
    regsub -- --VERSION-NUMBER-- $line $nVersion line
    regsub -- --SOURCE-ID--      $line "$zSourceId" line

    if {[regexp $varpattern $line] && ![regexp {^ *typedef} $line]} {
      set line "SQLITE_API $line"
    } else {
      if {[regexp $declpattern1 $line all rettype funcname rest] || \
          [regexp $declpattern2 $line all rettype funcname rest] || \
          [regexp $declpattern3 $line all rettype funcname rest] || \
          [regexp $declpattern4 $line all rettype funcname rest] || \
          [regexp $declpattern5 $line all rettype funcname rest]} {
        set line SQLITE_API
        append line " " [string trim $rettype]
        if {[string index $rettype end] ne "*"} {
          append line " "
        }
        if {$useapicall} {
          if {[lsearch -exact $cdecllist $funcname] >= 0} {
            append line SQLITE_CDECL " "
          } else {
            append line SQLITE_APICALL " "
          }
        }
        append line $funcname $rest
      }
    }
    if {$useapicall} {
      set line [string map [list (*sqlite3_syscall_ptr) \
          "(SQLITE_SYSAPI *sqlite3_syscall_ptr)"] $line]
      regsub {\(\*} $line {(SQLITE_CALLBACK *} line
    }
    puts $out $line
  }
  close $in
  if {![regexp {sqlite\.h\.in} $file]} {
    puts $out "/******** End of [file tail $file] *********/"
  }
}
puts $out "#endif /* SQLITE3_H */"
