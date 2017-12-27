#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

//#define DEBUG_PRINT

sqlite3 *db;                  /* The SQLite database connection */
const char *zLastError;

const char testStatements1[] =	"insert into test (a) values ('te\0st');";
const char testStatements2[] =	"create table test (a char);";
const char testStatements3[] =	"pragma allow_inline_nul=on;";
const char testStatements4[] =	"insert into test (a) values (?);";

const char testStatements5a[] =	"insert into test (a) values ('te\0zz');";
const char testStatements5b[] =	"insert into test (a) values ('te\0yy');";
const char testStatements5c[] =	"insert into test (a) values ('te\0bb');";
const char testStatements5d[] =	"insert into test (a) values ('te\0mm');";


/* Print an error message and exit */
static void fatal_error(const char *zMsg, ...){
  va_list ap;
  va_start(ap, zMsg);
  vfprintf(stderr, zMsg, ap);
  va_end(ap);
  exit(1);
}

void ExpectFail( const char *sql, int len, const char *expect, int nLine ) {
	int rc;
	sqlite3_stmt *pStmt;

	rc = sqlite3_prepare_v2(db, sql, len, &pStmt, 0);
	if( rc == SQLITE_OK )
		fatal_error( "Default sqlite should have returned an error.\n" );
	zLastError = sqlite3_errmsg( db );
	if( strcmp( zLastError, expect ) != 0 ) {
		//const char *sql = sqlite3_sql( pStmt );
		fatal_error( "(1)expected: [%s] got [%s]\n", expect, zLastError );
	}
	sqlite3_finalize( pStmt );

}

void ExpectSuccess( const char *sql, int len, int nLine ) {
	int rc;
	sqlite3_stmt *pStmt;
	rc = sqlite3_prepare_v2(db, sql, len, &pStmt, 0);
	if( rc != SQLITE_OK ) {
		zLastError = sqlite3_errmsg( db );
		fatal_error( "(%d-prep)unexpected error: [%s]\n", nLine, zLastError );
	}
	rc = sqlite3_step( pStmt );
	if( rc != SQLITE_OK && rc != SQLITE_DONE ) {
		zLastError = sqlite3_errmsg( db );
		fatal_error( "(%d-step)unexpected error: %d[%s]\n", nLine, rc, zLastError );
	}
	{
		int _len;
		const char *_sql = sqlite3_sql_utf8( pStmt, &_len );
#ifdef DEBUG_PRINT
		printf( "got:" );
		fwrite( _sql, 1, _len, stdout );
		printf( "\n" );
#endif
	}
	sqlite3_finalize( pStmt );
}


void ExpectBindSuccess( const char *sql, int len, int nLine, char *param, int paramLen ) {
	int rc;
	sqlite3_stmt *pStmt;
	rc = sqlite3_prepare_v2(db, sql, len, &pStmt, 0);
	if( rc != SQLITE_OK ) {
		zLastError = sqlite3_errmsg( db );
		fatal_error( "(%d-prep)unexpected error: [%s]\n", nLine, zLastError );
	}
	sqlite3_bind_text( pStmt, 1, param, paramLen, NULL );
	rc = sqlite3_step( pStmt );
	if( rc != SQLITE_OK && rc != SQLITE_DONE ) {
		zLastError = sqlite3_errmsg( db );
		fatal_error( "(%d-step)unexpected error: %d[%s]\n", nLine, rc, zLastError );
	}
	{
		int _len;
		const char *_sql = sqlite3_expanded_sql_utf8( pStmt, &_len );
#ifdef DEBUG_PRINT
		printf( "got:" );
		fwrite( _sql, 1, _len, stdout );
		printf( "\n" );
#endif
		if( memcmp( _sql, "insert into test (a) values ('Another\0Test');", _len ) != 0 )
			fatal_error( "expanded_sql is not correct." );
	}
	sqlite3_finalize( pStmt );
}

void GetResults( int nLine ) {
	int rc;
	int row = 0;
	sqlite3_stmt *pStmt;
	const char *value;
	int valueLen;
	rc = sqlite3_prepare_v2(db, "select * from test", -1, &pStmt, 0);
	if( rc != SQLITE_OK ) {
		zLastError = sqlite3_errmsg( db );
		fatal_error( "(%d-prep)unexpected error: [%s]\n", nLine, zLastError );
	}
	do {
		rc = sqlite3_step( pStmt );
		if( rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW ) {
			zLastError = sqlite3_errmsg( db );
			fatal_error( "(%d-step)unexpected error: %d[%s]\n", nLine, rc, zLastError );
		}
		value = (const char *)sqlite3_column_text( pStmt, 0 );
		valueLen = sqlite3_column_bytes( pStmt, 0 );
		switch( row ) {
		case 0:
			if( valueLen != 5 || memcmp( value, "te\0st", 5 ) != 0 )
				fatal_error( "Unexpected result: expected test %d  %.*s\n", valueLen, valueLen, value );
			break;
		case 1:
			if( valueLen != 12 || memcmp( value, "Another\0Test", 12 ) != 0 )
				fatal_error( "Unexpected result: expected Another test  %d  %.*s\n", valueLen, valueLen, value );
			break;
		}
		row++;
	} while( rc != SQLITE_DONE );
	{
		int _len;
		const char *_sql = sqlite3_expanded_sql_utf8( pStmt, &_len );
#ifdef DEBUG_PRINT
		printf( "got:" );
		fwrite( _sql, 1, _len, stdout );
		printf( "\n" );
#endif
	}
	sqlite3_finalize( pStmt );
}

void GetOrderedResults( int nLine ) {
	int rc;
	int row = 0;
	sqlite3_stmt *pStmt;
	const char *value;
	int valueLen;
	rc = sqlite3_prepare_v2(db, "select * from test order by a", -1, &pStmt, 0);
	if( rc != SQLITE_OK ) {
		zLastError = sqlite3_errmsg( db );
		fatal_error( "(%d-prep)unexpected error: [%s]\n", nLine, zLastError );
	}
	do {
		rc = sqlite3_step( pStmt );
		if( rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW ) {
			zLastError = sqlite3_errmsg( db );
			fatal_error( "(%d-step)unexpected error: %d[%s]\n", nLine, rc, zLastError );
		}
		value = (const char *)sqlite3_column_text( pStmt, 0 );
		valueLen = sqlite3_column_bytes( pStmt, 0 );
		fwrite( value, 1, valueLen, stdout );
		printf( "\n" );
		row++;
	} while( rc != SQLITE_DONE );
	{
		int _len;
		const char *_sql = sqlite3_expanded_sql_utf8( pStmt, &_len );
#ifdef DEBUG_PRINT
		printf( "got:" );
		fwrite( _sql, 1, _len, stdout );
		printf( "\n" );
#endif
	}
	sqlite3_finalize( pStmt );
}


int main( void ) {
	int rc; /* sqlite3 result code */
	unlink( "testdir/nultest.db" );
	rc = sqlite3_open( "testdir/nultest.db", &db);

	ExpectFail( testStatements1, sizeof( testStatements1 ), "unrecognized token: \"'te\"", __LINE__ );

	ExpectSuccess( testStatements2, sizeof( testStatements2 ), __LINE__ );
	ExpectSuccess( testStatements3, sizeof( testStatements3 ), __LINE__ );
	ExpectSuccess( testStatements1, sizeof( testStatements1 ), __LINE__ );

	ExpectBindSuccess( testStatements4, sizeof( testStatements4 ), __LINE__, "Another\0Test", 12 );

	GetResults( __LINE__ );

	ExpectSuccess( testStatements5a, sizeof( testStatements1 ), __LINE__ );
	ExpectSuccess( testStatements5b, sizeof( testStatements1 ), __LINE__ );
	ExpectSuccess( testStatements5c, sizeof( testStatements1 ), __LINE__ );
	ExpectSuccess( testStatements5d, sizeof( testStatements1 ), __LINE__ );

	//GetOrderedResults( __LINE__ );
	return 0;
}


