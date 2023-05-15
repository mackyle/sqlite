/*
** 2023 May 8
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file declares the interface of a simple resource management package
** which supports freeing of resources upon an abrupt, program-initiated
** termination of a function from somewhere in a call tree. The package is
** designed to be used with setjmp()/longjmp() to effect a pattern similar
** to the try/throw/catch available in some programming languages. (But see
** below regarding usage when the setmp()/longjmp() pair is unavailable.)
**
** The general scheme is that routines whose pre-return code might be
** bypassed, thereby leaking resources, do not rely on pre-return code
** to release locally held resources. Instead, they give ownership of
** such resources to this package, via xxxx_holder(...) calls, and use
** its holder_mark() and holder_free(...) functions to release locally
** acquired resources.
**
** For environments where setmp()/longjmp() are unavailable, (indicated by
** SHELL_OMIT_LONGJMP defined), the package substitutes a process exit for
** resumption of execution at a chosen code location. The resources in the
** package's held resource stack are still released. And the ability to
** free locally acquired resources as functions return is retained. This
** can simplify early function exit logic, but a body of code relying on
** process exit to ease error handling is unsuitable for use as a called
** routine within a larger application. That use is most of the reason for
** this package's existence.
*/

#ifndef RES_MANAGE_H
# define RES_MANAGE_H

#ifndef SHELL_OMIT_LONGJMP
# include <setjmp.h>
#endif
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "sqlite3.h"

/* Type used for marking/conveying positions within a held-resource stack */
typedef unsigned short ResourceMark;
typedef unsigned short ResourceCount;

/* Type used to record a possible succession of recovery destinations */
typedef struct RipStackDest {
  struct RipStackDest *pPrev;
  ResourceMark resDest;
#ifndef SHELL_OMIT_LONGJMP
  jmp_buf exeDest;
#endif
} RipStackDest;
#define RIP_STACK_DEST_INIT {0}

/* This macro effects stack mark and potential rip-back, keeping needed
** details in a RipStackDest object (independent of SHELL_OMIT_LONGJMP.) */
#ifndef SHELL_OMIT_LONGJMP
# define RIP_TO_HERE(RSD) (RSD.resDest=holder_mark(), setjmp(RSD.exeDest))
#else
# define RIP_TO_HERE(RSD) (RSD.resDest=holder_mark(), 0)
#endif

/* Type used for generic free functions (to fib about their signature.) */
typedef void (*GenericFreer)(void*);

/* Current position of the held-resource stack */
extern ResourceMark holder_mark();

/* Assure no allocation failure for some more xxx_holder() calls.
** Note that this call may fail with an OOM abrupt exit. */
extern void more_holders(ResourceCount more);

/* Lose one or more holders, without freeing anything. */
extern void* pop_holder(void);
extern void pop_holders(ResourceCount num);

/* Drop a holder while freeing its holdee. */
extern void release_holder(void);

/*
** Routines for holding resources on held-resource stack together
** with enough information for them to be freed by this package.
*/

/* xxxx_holder(...)
** The referenced objects are directly freed; they are stored in
** the heap with lifetime not bound to the caller's activation.
*/
/* anything together with arbitrary freeing function */
extern void* any_holder(void *pm, void (*its_freer)(void*));
/* anything in the malloc() heap */
extern void* mmem_holder(void *pm);
/* a C string in the malloc() heap */
extern char* mstr_holder(char *z);
/* some SQLite-allocated memory */
extern void* smem_holder(void *pm);
/* a C string in the SQLite heap */
extern char* sstr_holder(char *z);
/* a SQLite database "connection" */
extern void conn_holder(sqlite3 *pdb);
/* a SQLite prepared statement */
extern void stmt_holder(sqlite3_stmt *pstmt);
/* an open C runtime FILE */
extern void file_holder(FILE *);
#if (!defined(_WIN32) && !defined(WIN32)) || !SQLITE_OS_WINRT
/* an open C runtime pipe */
extern void pipe_holder(FILE *);
#endif

/* xxxx_ptr_holder(...) or xxxx_ref_holder(...)
** The referenced objects are indirectly freed; they are stored
** in the caller's stack frame, with lifetime limited to the
** the caller's activation. It is a grave error to use these
** then fail to call holder_free() before returning. For abrupt
** exits, this condition is met because holder_free() is called
** by the abrupt exiter before the execution stack is stripped.
*/
typedef struct AnyResourceHolder {
  void *pAny;
  GenericFreer its_freer;
} AnyResourceHolder;

/* a reference to an AnyResourceHolder (whose storage is not managed) */
extern void any_ref_holder(AnyResourceHolder *parh);
/* a C string in the SQLite heap, reference to */
extern void sstr_ptr_holder(char **pz);
/* a SQLite prepared statement, reference to */
extern void stmt_ptr_holder(sqlite3_stmt **ppstmt);
#ifdef SHELL_MANAGE_TEXT
/* a ShellText object, reference to (storage for which not managed) */
static void text_ref_holder(ShellText *);
#endif

/* Take back a held resource pointer, leaving held as NULL. (no-op) */
extern void* take_held(ResourceMark mark, ResourceCount offset);

/* Swap a held resource pointer for a new one. */
extern void* swap_held(ResourceMark mark, ResourceCount offset, void *pNew);

/* Free all held resources in excess of given resource stack mark.
** Return count of number actually freed (rather than being 0.) */
extern int holder_free(ResourceMark mark);

/* Remember execution and resource stack postion/state. This determines
** how far these stacks may be stripped should quit_moan(...) be called.
*/
extern void register_exit_ripper(RipStackDest *);
/* Forget whatever register_exit_ripper() has been recorded. */
extern void forget_exit_ripper(RipStackDest *);

/* Strip resource stack and execute previously registered longjmp() as
** previously prepared by register_exit_ripper() call. Or, if no such
** prep done (or possible), strip the whole stack and exit the process.
*/
extern void quit_moan(const char *zMoan, int errCode);

/* What the complaint will be for OOM failures and abrupt exits. */
extern const char *resmanage_oom_message;

#ifdef __cplusplus
}  /* End of the 'extern "C"' block */
#endif
#endif /* RES_MANAGE_H */
