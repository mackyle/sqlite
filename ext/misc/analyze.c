/*
** 2026-04-13
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Partial reimplement of the sqlite3_analyzer utility program as
** loadable SQL function.
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

/*
** State information for the analysis
*/
typedef struct Analysis Analysis;
struct Analysis {
  sqlite3 *db;               /* Database connection */
  sqlite3_context *context;  /* SQL function context */
  sqlite3_str *pOut;         /* Write output here */
  char *zSU;                 /* Name of the temp.space_used table */
  const char *zSchema;       /* Schema to be analyzed */
};

/*
** Free all memory associated with the Analysis objecct
*/
static void analysisFree(Analysis *p){
  if( p->zSU ){
    char *zSql = sqlite3_mprintf("DROP TABLE temp.%s;", p->zSU);
    if( zSql ){
      sqlite3_exec(p->db, zSql, 0, 0, 0);
      sqlite3_free(zSql);
    }
  }
  sqlite3_str_free(p->pOut);
  sqlite3_free(p->zSU);
  memset(p, 0, sizeof(*p));
}

/*
** Report an OOM
*/
static void analysisOom(Analysis *p){
  sqlite3_result_error_nomem(p->context);
  analysisFree(p);
}

/*
** Prepare and return an SQL statement.
*/
static sqlite3_stmt *analysisVPrep(Analysis *p, const char *zFmt, va_list ap){
  char *zSql;
  int rc;
  sqlite3_stmt *pStmt = 0;
  zSql = sqlite3_vmprintf(zFmt, ap);
  if( zSql==0 ){ analysisOom(p); return 0; }
  rc = sqlite3_prepare_v2(p->db, zSql, -1, &pStmt, 0);
  if( rc ){
    char *zErr = sqlite3_mprintf("SQL parse error: %s\nOriginal SQL: %s",
                                 sqlite3_errmsg(p->db), zSql);
    sqlite3_result_error(p->context, zErr, -1);
    sqlite3_free(zErr);
    sqlite3_finalize(pStmt);
    analysisFree(p);
    pStmt = 0;
  }
  sqlite3_free(zSql);
  return pStmt;
}
static sqlite3_stmt *analysisPrepare(Analysis *p, const char *zFormat, ...){
  va_list ap;
  sqlite3_stmt *pStmt = 0;
  va_start(ap, zFormat);
  pStmt = analysisVPrep(p,zFormat,ap);
  va_end(ap);
  return pStmt;
}

/*
** Run SQL.  Return the number of errors. 
*/
static int analysisSql(Analysis *p, const char *zFormat, ...){
  va_list ap;
  int rc;
  sqlite3_stmt *pStmt = 0;
  va_start(ap, zFormat);
  pStmt = analysisVPrep(p,zFormat,ap);
  va_end(ap);
  if( pStmt==0 ) return 1;
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){}
  if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
  }else{
    char *zErr = sqlite3_mprintf("SQL run-time error: %s\nOriginal SQL: %s",
                                 sqlite3_errmsg(p->db), sqlite3_sql(pStmt));
    sqlite3_result_error(p->context, zErr, -1);
    sqlite3_free(zErr);
    analysisFree(p);
  }
  sqlite3_finalize(pStmt);
  return rc;
}

/*
** SQL Function:   analyze(SCHEMA)
**
** Analyze the database schema named in the argument.  Return text
** containing the analysis.
*/
static void analyzeFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  int rc;
  sqlite3_stmt *pStmt;
  int n;
  Analysis s;
  sqlite3_uint64 r[2];

  memset(&s, 0, sizeof(s));
  s.db = sqlite3_context_db_handle(context);
  s.context = context;
  s.pOut = sqlite3_str_new(0);
  if( s.pOut==0 ){ analysisOom(&s); return; }
  s.zSchema = (const char*)sqlite3_value_text(argv[0]);
  sqlite3_randomness(sizeof(r), &r);
  s.zSU = sqlite3_mprintf("analysis%016x%016x", r[0], r[1]);
  if( s.zSU==0 ){ analysisOom(&s); return; }

  /* The s.zSU table contains the data used for the analysis.
  ** The table name contains 128-bits of randomness to avoid
  ** collisions with preexisting tables in temp.
  */
  rc = analysisSql(&s,
    "CREATE TABLE temp.%s(\n"
    "   name text,                -- A table or index\n"
    "   tblname text,             -- Table that owns name\n"
    "   is_index boolean,         -- TRUE if it is an index\n"
    "   is_without_rowid boolean, -- TRUE if WITHOUT ROWID table\n"
    "   nentry int,               -- Number of entries in the BTree\n"
    "   leaf_entries int,         -- Number of leaf entries\n"
    "   depth int,                -- Depth of the b-tree\n"
    "   payload int,              -- Total data stored in this table/index\n"
    "   ovfl_payload int,         -- Total data stored on overflow pages\n"
    "   ovfl_cnt int,             -- Number of entries that use overflow\n"
    "   mx_payload int,           -- Maximum payload size\n"
    "   int_pages int,            -- Interior pages used\n"
    "   leaf_pages int,           -- Leaf pages used\n"
    "   ovfl_pages int,           -- Overflow pages used\n"
    "   int_unused int,           -- Unused bytes on interior pages\n"
    "   leaf_unused int,          -- Unused bytes on primary pages\n"
    "   ovfl_unused int           -- Unused bytes on overflow pages\n"
    ");",
    s.zSU
  );
  if( rc ) return;

  /* Populate the s.zSU table
  */
  rc = analysisSql(&s,
    "WITH\n"
    "  allidx(idxname) AS (\n"
    "    SELECT name FROM main.sqlite_schema WHERE type='index'\n"
    "  ),\n"
    "  allobj(allname,tblname,isidx,isworowid) AS (\n"
    "    SELECT 'sqlite_schema',\n"
    "           'sqlite_schema',\n"
    "           0,\n"
    "           0\n"
    "    UNION ALL\n"
    "    SELECT name,\n"
    "           tbl_name,\n"
    "           type='index',\n"
    "           EXISTS(SELECT 1\n"
    "                    FROM pragma_index_list(sqlite_schema.name,%Q)\n"
    "                   WHERE pragma_index_list.origin='pk'\n"
    "                     AND pragma_index_list.name NOT IN allidx)\n"
    "      FROM \"%w\".sqlite_schema\n"
    "  )\n"
    "INSERT INTO temp.%s\n"
    "  SELECT\n"
    "    allname,\n"
    "    tblname,\n"
    "    isidx,\n"
    "    isworowid,\n"
    "    sum(ncell),\n"
    "    sum((pagetype='leaf')*ncell),\n"
    "    max((length(if(path GLOB '*+*','',path))+3)/4),\n"
    "    sum(payload),\n"
    "    sum((pagetype='overflow')*payload),\n"
    "    sum(path GLOB '*+000000'),\n"
    "    max(mx_payload),\n"
    "    sum(pagetype='internal'),\n"
    "    sum(pagetype='leaf'),\n"
    "    sum(pagetype='overflow'),\n"
    "    sum((pagetype='internal')*unused),\n"
    "    sum((pagetype='leaf')*unused),\n"
    "    sum((pagetype='overflow')*unused)\n"
    "  FROM allobj CROSS JOIN dbstat(%Q) \n"
    "  WHERE dbstat.name=allobj.allname\n"
    "  GROUP BY allname;\n",
    s.zSchema,   /* pragma_index_list(...,%Q) */
    s.zSchema,   /* %w.sqlite_schema */
    s.zSU,       /* INTO temp.%s */
    s.zSchema    /* JOIN dbstat(%Q) */
  );
  if( rc ) return;

  /* TBD: Generate report text here */

  /* Append SQL statements that will recreate the raw data used for
  ** the analysis.
  */
  sqlite3_str_appendf(s.pOut,
    "BEGIN;\n"
    "CREATE TABLE space_used(\n"
    "   name text,                -- A table or index\n"                /* 0 */
    "   tblname text,             -- Table that owns name\n"            /* 1 */
    "   is_index boolean,         -- TRUE if it is an index\n"          /* 2 */
    "   is_without_rowid boolean, -- TRUE if WITHOUT ROWID table\n"     /* 3 */
    "   nentry int,               -- Number of entries in the BTree\n"  /* 4 */
    "   leaf_entries int,         -- Number of leaf entries\n"          /* 5 */
    "   depth int,                -- Depth of the b-tree\n"             /* 6 */
    "   payload int,              -- Total data in this table/index\n"  /* 7 */
    "   ovfl_payload int,         -- Total data on overflow pages\n"    /* 8 */
    "   ovfl_cnt int,             -- Entries that use overflow\n"       /* 9 */
    "   mx_payload int,           -- Maximum payload size\n"            /* 10 */
    "   int_pages int,            -- Interior pages used\n"             /* 11 */
    "   leaf_pages int,           -- Leaf pages used\n"                 /* 12 */
    "   ovfl_pages int,           -- Overflow pages used\n"             /* 13 */
    "   int_unused int,           -- Unused bytes on interior pages\n"  /* 14 */
    "   leaf_unused int,          -- Unused bytes on primary pages\n"   /* 15 */
    "   ovfl_unused int           -- Unused bytes on overflow pages\n"  /* 16 */
    ");\n"
    "INSERT INTO space_used VALUES\n"
  );
  pStmt = analysisPrepare(&s,
     "SELECT quote(name), quote(tblname),\n"                        /* 0..1 */
     "       is_index, is_without_rowid, nentry, leaf_entries,\n"   /* 2..5 */
     "       depth, payload, ovfl_payload, ovfl_cnt, mx_payload,\n" /* 6..10 */
     "       int_pages, leaf_pages, ovfl_pages, int_unused,\n"      /* 11..14 */
     "       leaf_unused, ovfl_unused\n"                            /* 15..16 */
     "  FROM temp.%s;",
     s.zSU);
  if( pStmt==0 ) return;
  n = 0;
  while( sqlite3_step(pStmt)==SQLITE_ROW ){
    if( n++ ) sqlite3_str_appendf(s.pOut,",\n");
    sqlite3_str_appendf(s.pOut,
      " (%s,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
      "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld)",
      sqlite3_column_text(pStmt, 0),
      sqlite3_column_text(pStmt, 1),
      sqlite3_column_int64(pStmt, 2),
      sqlite3_column_int64(pStmt, 3),
      sqlite3_column_int64(pStmt, 4),
      sqlite3_column_int64(pStmt, 5),
      sqlite3_column_int64(pStmt, 6),
      sqlite3_column_int64(pStmt, 7),
      sqlite3_column_int64(pStmt, 8),
      sqlite3_column_int64(pStmt, 9),
      sqlite3_column_int64(pStmt, 10),
      sqlite3_column_int64(pStmt, 11),
      sqlite3_column_int64(pStmt, 12),
      sqlite3_column_int64(pStmt, 13),
      sqlite3_column_int64(pStmt, 14),
      sqlite3_column_int64(pStmt, 15),
      sqlite3_column_int64(pStmt, 16));
  }
  sqlite3_str_appendf(s.pOut,"\nCOMMIT;\n");
  sqlite3_finalize(pStmt);

  if( sqlite3_str_length(s.pOut) ){
    sqlite3_result_text(context, sqlite3_str_finish(s.pOut), -1,
                        sqlite3_free);
    s.pOut = 0;
  }
  analysisFree(&s);
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_analyze_init(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = sqlite3_create_function(db, "analyze", 1,
                   SQLITE_UTF8|SQLITE_INNOCUOUS|SQLITE_DETERMINISTIC,
                   0, analyzeFunc, 0, 0);
  return rc;
}
