/*
** 2025-10-20
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Header file for the Result-Format or "resfmt" utility library for SQLite.
** See the resfmt.md documentation for additional information.
*/
#ifndef SQLITE_QRF_H
#define SQLITE_QRF_H
#include <stdlib.h>
#include "sqlite3.h"

/*
** Specification used by clients to define the output format they want
*/
typedef struct sqlite3_qrf_spec sqlite3_qrf_spec;
struct sqlite3_qrf_spec {
  unsigned char iVersion;     /* Version number of this structure */
  unsigned char eStyle;       /* Formatting style.  "box", "csv", etc... */
  unsigned char eEsc;         /* How to escape control characters in text */
  unsigned char eText;        /* Quoting style for text */
  unsigned char eBlob;        /* Quoting style for BLOBs */
  unsigned char bColumnNames; /* True to show column names */
  unsigned char bWordWrap;    /* Try to wrap on word boundaries */
  unsigned char bTextJsonb;   /* Render JSONB blobs as JSON text */
  short int mxWidth;          /* Maximum width of any column */
  int nWidth;                 /* Number of column width parameters */
  short int *aWidth;          /* Column widths */
  const char *zColumnSep;     /* Alternative column separator */
  const char *zRowSep;        /* Alternative row separator */
  const char *zTableName;     /* Output table name */
  const char *zNull;          /* Rendering of NULL */
  char *(*xRender)(void*,sqlite3_value*);                /* Render a value */
  ssize_t (*xWrite)(void*,const unsigned char*,size_t);  /* Write callback */
  void *pRenderArg;           /* First argument to the xRender callback */
  void *pWriteArg;            /* First argument to the xWrite callback */
  char **pzOutput;            /* Storage location for output string */
  /* Additional fields may be added in the future */
};

/*
** Range of values for sqlite3_qrf_spec.aWidth[] entries:
*/
#define QRF_MX_WIDTH    (10000)
#define QRF_MN_WIDTH    (-10000)
#define QRF_MINUS_ZERO  (-32768)    /* Special value meaning -0 */

/*
** Interfaces
*/
int sqlite3_format_query_result(
  sqlite3_stmt *pStmt,             /* SQL statement to run */
  const sqlite3_qrf_spec *pSpec,   /* Result format specification */
  char **pzErr                     /* OUT: Write error message here */
);

/*
** Output styles:
*/
#define QRF_STYLE_List      0 /* One record per line with a separator */
#define QRF_STYLE_Line      1 /* One column per line. */
#define QRF_STYLE_Html      2 /* Generate an XHTML table */
#define QRF_STYLE_Json      3 /* Output is a list of JSON objects */
#define QRF_STYLE_Insert    4 /* Generate SQL "insert" statements */
#define QRF_STYLE_Csv       5 /* Comma-separated-value */
#define QRF_STYLE_Quote     6 /* SQL-quoted, comma-separated */
#define QRF_STYLE_Explain   7 /* EXPLAIN output */
#define QRF_STYLE_ScanExp   8 /* EXPLAIN output with vm stats */
#define QRF_STYLE_EQP       9 /* Format EXPLAIN QUERY PLAN output */
#define QRF_STYLE_Markdown 10 /* Markdown formatting */
#define QRF_STYLE_Column   11 /* One record per line in neat columns */
#define QRF_STYLE_Table    12 /* MySQL-style table formatting */
#define QRF_STYLE_Box      13 /* Unicode box-drawing characters */
#define QRF_STYLE_Count    14 /* Output only a count of the rows of output */
#define QRF_STYLE_Off      15 /* No query output shown */

/*
** Quoting styles for text.
** Allowed values for sqlite3_qrf_spec.eText
*/
#define QRF_TEXT_Off     0 /* Literal text */
#define QRF_TEXT_Sql     1 /* Quote as an SQL literal */
#define QRF_TEXT_Csv     2 /* CSV-style quoting */
#define QRF_TEXT_Html    3 /* HTML-style quoting */
#define QRF_TEXT_Tcl     4 /* C/Tcl quoting */
#define QRF_TEXT_Json    5 /* JSON quoting */

/*
** Quoting styles for BLOBs
** Allowed values for sqlite3_qrf_spec.eBlob
*/
#define QRF_BLOB_Auto    0 /* Determine BLOB quoting using eText */
#define QRF_BLOB_Text    1 /* Display content exactly as it is */
#define QRF_BLOB_Sql     2 /* Quote as an SQL literal */
#define QRF_BLOB_Hex     3 /* Hexadecimal representation */
#define QRF_BLOB_Tcl     4 /* "\000" notation */
#define QRF_BLOB_Json    5 /* A JSON string */

/*
** Control-character escape modes.
** Allowed values for sqlite3_qrf_spec.eEscape
*/
#define QRF_ESC_Off     0 /* Do not escape control characters */
#define QRF_ESC_Ascii   1 /* Unix-style escapes.  Ex: U+0007 shows ^G */
#define QRF_ESC_Symbol  2 /* Unicode escapes. Ex: U+0007 shows U+2407 */

#endif /* !defined(SQLITE_QRF_H) */
