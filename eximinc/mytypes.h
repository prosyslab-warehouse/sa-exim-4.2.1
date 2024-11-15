/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) University of Cambridge 1995 - 2003 */
/* See the file NOTICE for conditions of use and distribution. */


/* This header file contains type definitions and macros that I use as
"standard" in the code of Exim and its utilities. Make it idempotent because
local_scan.h includes it and exim.h includes them both (to get this earlier). */

#ifndef MYTYPES_H
#define MYTYPES_H


#define FALSE         0
#define TRUE          1
#define TRUE_UNSET    2


/* If gcc is being used to compile Exim, we can use its facility for checking
the arguments of printf-like functions. This is done by a macro. */

#ifdef __GNUC__
#define PRINTF_FUNCTION  __attribute__((format(printf,1,2)))
#else
#define PRINTF_FUNCTION
#endif


/* Some operating systems (naughtily, imo) include a definition for "uchar" in
the standard header files, so we use "uschar". Solaris has u_char in
sys/types.h. This is just a typing convenience, of course. */

typedef int BOOL;
typedef unsigned char uschar;


/* These macros save typing for the casting that is needed to cope with the
mess that is "char" in ISO/ANSI C. Having now been bitten enough times by
systems where "char" is actually signed, I've converted Exim to use entirely
unsigned chars, except in a few special places such as arguments that are
almost always literal strings. */

#define CS   (char *)
#define CSS  (char **)
#define US   (unsigned char *)
#define USS  (unsigned char **)

/* The C library string functions expect "char *" arguments. Use macros to
avoid having to write a cast each time. We do this for string and file
functions; for other calls to external libraries (which are on the whole
special-purpose) we just use casts. */


#define Uatoi(s)           atoi(CS(s))
#define Uatol(s)           atol(CS(s))
#define Uchdir(s)          chdir(CS(s))
#define Uchmod(s,n)        chmod(CS(s),n)
#define Uchown(s,n,m)      chown(CS(s),n,m)
#define Ufgets(b,n,f)      fgets(CS(b),n,f)
#define Ufopen(s,t)        fopen(CS(s),CS(t))
#define Ulink(s,t)         link(CS(s),CS(t))
#define Ulstat(s,t)        lstat(CS(s),t)
#ifdef O_BINARY                                       /* This is for Cygwin,  */
#define Uopen(s,n,m)       open(CS(s),(n)|O_BINARY,m) /* where all files must */
#else                                                 /* be opened as binary  */
#define Uopen(s,n,m)       open(CS(s),n,m)            /* to avoid problems    */
#endif                                                /* with CRLF endings.   */
#define Uread(f,b,l)       read(f,CS(b),l)
#define Urename(s,t)       rename(CS(s),CS(t))
#define Ustat(s,t)         stat(CS(s),t)
#define Ustrcat(s,t)       strcat(CS(s),CS(t))
#define Ustrchr(s,n)       US strchr(CS(s),n)
#define Ustrcmp(s,t)       strcmp(CS(s),CS(t))
#define Ustrcpy(s,t)       strcpy(CS(s),CS(t))
#define Ustrcspn(s,t)      strcspn(CS(s),CS(t))
#define Ustrftime(s,m,f,t) strftime(CS(s),m,f,t)
#define Ustrlen(s)         (int)strlen(CS(s))
#define Ustrncat(s,t,n)    strncat(CS(s),CS(t),n)
#define Ustrncmp(s,t,n)    strncmp(CS(s),CS(t),n)
#define Ustrncpy(s,t,n)    strncpy(CS(s),CS(t),n)
#define Ustrpbrk(s,t)      strpbrk(CS(s),CS(t))
#define Ustrrchr(s,n)      US strrchr(CS(s),n)
#define Ustrspn(s,t)       strspn(CS(s),CS(t))
#define Ustrstr(s,t)       US strstr(CS(s),CS(t))
#define Ustrtod(s,t)       strtod(CS(s),CSS(t))
#define Ustrtol(s,t,b)     strtol(CS(s),CSS(t),b)
#define Ustrtoul(s,t,b)    strtoul(CS(s),CSS(t),b)
#define Uunlink(s)         unlink(CS(s))

#endif

/* End of mytypes.h */
