#!sqlite3
#
# 2026-04-11
#
# The author disclaims copyright to this source code.  In place of
# a legal notice, here is a blessing:
#
#    May you do good and not evil.
#    May you find forgiveness for yourself and forgive others.
#    May you share freely, never taking more than you give.
#
#***********************************************************************
#
# Tests for the .prompt command and prompt rendering.
#
#   ./sqlite3 test/shell-prompt.sql
#

.testcase setup
.open -new test.db
.mode list -quote off -escape ascii
.check ''

.testcase 100
.prompt
.check ''

.testcase 110
.prompt --show
.check <<END
Main prompt:  'SQLite /f> '
Continuation: '/B.../H> '
END
.testcase 111
.prompt 'abc> ' '123> ' -show
.check <<END
Main prompt:  'abc> '
Continuation: '123> '
END
.testcase 112
.prompt --  --first --second
.prompt --show
.check <<END
Main prompt:  '--first'
Continuation: '--second'
END
.testcase 113
.prompt --reset --show
.check <<END
Main prompt:  'SQLite /f> '
Continuation: '/B.../H> '
END



.testcase 1000
SELECT shell_prompt_test(NULL);
.check 'SQLite test.db> ';
.testcase 1001
SELECT shell_prompt_test(NULL,'SELECT');
.check '          ...;> ';
.testcase 1002
SELECT shell_prompt_test(NULL,'SELECT ((("');
.check '      ...")));> ';
.testcase 1003
SELECT shell_prompt_test(NULL,'SELECT ((()[');
.check '       ...]));> ';
.testcase 1004
SELECT shell_prompt_test(NULL,'SELECT ''');
.check "         ...';> ";
.testcase 1005
SELECT shell_prompt_test(NULL,'CREATE TRIGGER t1 BEGIN');
.check "      ...;END;> ";
.testcase 1006
SELECT shell_prompt_test(NULL,'CREATE TRIGGER t1 BEGIN SELECT ((([');
.check "  ...])));END;> ";
.testcase 1007
SELECT shell_prompt_test(NULL,'CREATE TRIGGER t1 BEGIN SELECT ((/*a(((''bc');
.check "  ...*/));END;> ";
.testcase 1008
SELECT shell_prompt_test(NULL,'CREATE TRIGGER t1 BEGIN SELECT 1;');
.check "       ...END;> ";

.testcase 2000
.prompt 'SQLite/x-txn$/:>/; '
SELECT shell_prompt_test(NULL);
.check 'SQLite> ';
.testcase 2001
BEGIN;
SELECT shell_prompt_test(NULL);
.check 'SQLite-txn$ ';
.testcase 2002
ROLLBACK;
SELECT shell_prompt_test(NULL);
.check 'SQLite> ';
.testcase 2003
.prompt -- '--show '
SELECT shell_prompt_test(NULL);
.check '--show ';
.testcase 2004
.prompt --reset
SELECT shell_prompt_test(NULL);
.check 'SQLite test.db> ';

.testcase
