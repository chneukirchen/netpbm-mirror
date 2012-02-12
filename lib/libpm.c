/**************************************************************************
                                  libpm.c
***************************************************************************
  This file contains fundamental libnetpbm services.

  Some of the subroutines in this library are intended and documented
  for use by Netpbm users, but most of them are just used by other
  Netpbm library subroutines.
**************************************************************************/

#define _BSD_SOURCE          /* Make sure strdup is in string.h */
#define _XOPEN_SOURCE 500    /* Make sure ftello, fseeko are defined */

#include "netpbm/pm_config.h"

#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <time.h>
#include <limits.h>
#if HAVE_FORK
#include <sys/wait.h>
#endif
#include <sys/types.h>

#include "netpbm/pm_c_util.h"
#include "netpbm/mallocvar.h"
#include "netpbm/version.h"
#include "netpbm/nstring.h"
#include "netpbm/shhopt.h"
#include "compile.h"

#include "netpbm/pm.h"

/* The following are set by pm_init(), then used by subsequent calls to other
   pm_xxx() functions.
   */
const char * pm_progname;

int pm_plain_output;
    /* Boolean: programs should produce output in plain format */

static bool pm_showmessages;  
    /* Programs should display informational messages (because the user didn't
       specify the --quiet option).
    */
static jmp_buf * pm_jmpbufP = NULL;
    /* A description of the point to which the program should hyperjump
       if a libnetpbm function encounters an error (libnetpbm functions
       don't normally return in that case).

       User sets this to something in his own extra-library context.
       Libnetpbm routines that have something that needs to be cleaned up
       preempt it.

       NULL, which is the default value, means when a libnetpbm function
       encounters an error, it causes the process to exit.
    */
static pm_usererrormsgfn * userErrorMsgFn = NULL;
    /* A function to call to issue an error message.

       NULL means use the library default: print to Standard Error
    */

static pm_usermessagefn * userMessageFn = NULL;
    /* A function to call to issue an error message.

       NULL means use the library default: print to Standard Error
    */



void
pm_setjmpbuf(jmp_buf * const jmpbufP) {
    pm_jmpbufP = jmpbufP;
}



void
pm_setjmpbufsave(jmp_buf *  const jmpbufP,
                 jmp_buf ** const oldJmpbufPP) {

    *oldJmpbufPP = pm_jmpbufP;
    pm_jmpbufP = jmpbufP;
}



void
pm_longjmp(void) {

    if (pm_jmpbufP)
        longjmp(*pm_jmpbufP, 1);
    else
        exit(1);
}



void
pm_fork(int *         const iAmParentP,
        pid_t *       const childPidP,
        const char ** const errorP) {
/*----------------------------------------------------------------------------
   Same as POSIX fork, except with a nicer interface and works
   (fails cleanly) on systems that don't have POSIX fork().
-----------------------------------------------------------------------------*/
#if HAVE_FORK
    int rc;

    rc = fork();

    if (rc < 0) {
        pm_asprintf(errorP, "Failed to fork a process.  errno=%d (%s)",
                    errno, strerror(errno));
    } else {
        *errorP = NULL;

        if (rc == 0) {
            *iAmParentP = FALSE;
        } else {
            *iAmParentP = TRUE;
            *childPidP = rc;
        }
    }
#else
    asprintfN(errorP, "Cannot fork a process, because this system does "
              "not have POSIX fork()");
#endif
}



void
pm_waitpid(pid_t         const pid,
           int *         const statusP,
           int           const options,
           pid_t *       const exitedPidP,
           const char ** const errorP) {

#if HAVE_FORK
    pid_t rc;
    rc = waitpid(pid, statusP, options);
    if (rc == (pid_t)-1) {
        pm_asprintf(errorP, "Failed to wait for process exit.  "
                    "waitpid() errno = %d (%s)",
                    errno, strerror(errno));
    } else {
        *exitedPidP = rc;
        *errorP = NULL;
    }
#else
    pm_error("INTERNAL ERROR: Attempt to wait for a process we created on "
             "a system on which we can't create processes");
#endif
}



void
pm_waitpidSimple(pid_t const pid) {

    int status;
    pid_t exitedPid;
    const char * error;

    pm_waitpid(pid, &status, 0, &exitedPid, &error);

    if (error) {
        pm_errormsg("%s", error);
        pm_strfree(error);
        pm_longjmp();
    } else {
        assert(exitedPid != 0);
    }
}



void
pm_setusererrormsgfn(pm_usererrormsgfn * fn) {

    userErrorMsgFn = fn;
}



void
pm_setusermessagefn(pm_usermessagefn * fn) {

    userMessageFn = fn;
}



void
pm_usage(const char usage[]) {
    pm_error("usage:  %s %s", pm_progname, usage);
}



void PM_GNU_PRINTF_ATTR(1,2)
pm_message(const char format[], ...) {

    va_list args;

    va_start(args, format);

    if (pm_showmessages) {
        const char * msg;
        pm_vasprintf(&msg, format, args);

        if (userMessageFn)
            userMessageFn(msg);
        else
            fprintf(stderr, "%s: %s\n", pm_progname, msg);

        pm_strfree(msg);
    }
    va_end(args);
}



static void
errormsg(const char * const msg) {

    if (userErrorMsgFn)
        userErrorMsgFn(msg);
    else
        fprintf(stderr, "%s: %s\n", pm_progname, msg);
}



void PM_GNU_PRINTF_ATTR(1,2)
pm_errormsg(const char format[], ...) {

    va_list args;
    const char * msg;

    va_start(args, format);

    pm_vasprintf(&msg, format, args);
    
    errormsg(msg);

    pm_strfree(msg);

    va_end(args);
}



void PM_GNU_PRINTF_ATTR(1,2)
pm_error(const char format[], ...) {
    va_list args;
    const char * msg;

    va_start(args, format);

    pm_vasprintf(&msg, format, args);
    
    errormsg(msg);

    pm_strfree(msg);

    va_end(args);

    pm_longjmp();
}



static void *
mallocz(size_t const size) {

    return malloc(MAX(1, size));
}



void *
pm_allocrow(unsigned int const cols,
            unsigned int const size) {

    unsigned char * itrow;

    if (cols != 0 && UINT_MAX / cols < size)
        pm_error("Arithmetic overflow multiplying %u by %u to get the "
                 "size of a row to allocate.", cols, size);

    itrow = mallocz(cols * size);
    if (itrow == NULL)
        pm_error("out of memory allocating a row");

    return itrow;
}



void
pm_freerow(void * const itrow) {
    free(itrow);
}



char **
pm_allocarray(int const cols,
              int const rows,
              int const elementSize ) {
/*----------------------------------------------------------------------------
   This is for backward compatibility.  MALLOCARRAY2 is usually better.

   A problem with pm_allocarray() is that its return type is char **
   even though 'elementSize' can be other than 1.  So users have
   traditionally type cast the result.  In the old days, that was just
   messy; modern compilers can produce the wrong code if you do that.
-----------------------------------------------------------------------------*/
    char ** retval;
    void * result;

    pm_mallocarray2(&result, rows, cols, elementSize);

    if (result == NULL)
        pm_error("Failed to allocate a raster array of %u columns x %u rows",
                 cols, rows);

    retval = result;

    return retval;
}



void
pm_freearray(char ** const rowIndex, 
             int     const rows) {

    void * const rowIndexVoid = rowIndex;

    pm_freearray2(rowIndexVoid);
}



/* Case-insensitive keyword matcher. */

int
pm_keymatch(const char *       const strarg, 
            const char * const keywordarg, 
            int          const minchars) {
    int len;
    const char * keyword;
    const char * str;

    str = strarg;
    keyword = keywordarg;

    len = strlen( str );
    if ( len < minchars )
        return 0;
    while ( --len >= 0 )
        {
        register char c1, c2;

        c1 = *str++;
        c2 = *keyword++;
        if ( c2 == '\0' )
            return 0;
        if ( ISUPPER( c1 ) )
            c1 = tolower( c1 );
        if ( ISUPPER( c2 ) )
            c2 = tolower( c2 );
        if ( c1 != c2 )
            return 0;
        }
    return 1;
}


/* Log base two hacks. */

int
pm_maxvaltobits(int const maxval) {
    if ( maxval <= 1 )
        return 1;
    else if ( maxval <= 3 )
        return 2;
    else if ( maxval <= 7 )
        return 3;
    else if ( maxval <= 15 )
        return 4;
    else if ( maxval <= 31 )
        return 5;
    else if ( maxval <= 63 )
        return 6;
    else if ( maxval <= 127 )
        return 7;
    else if ( maxval <= 255 )
        return 8;
    else if ( maxval <= 511 )
        return 9;
    else if ( maxval <= 1023 )
        return 10;
    else if ( maxval <= 2047 )
        return 11;
    else if ( maxval <= 4095 )
        return 12;
    else if ( maxval <= 8191 )
        return 13;
    else if ( maxval <= 16383 )
        return 14;
    else if ( maxval <= 32767 )
        return 15;
    else if ( (long) maxval <= 65535L )
        return 16;
    else
        pm_error( "maxval of %d is too large!", maxval );
        return -1;  /* Should never come here */
}

int
pm_bitstomaxval(int const bits) {
    return ( 1 << bits ) - 1;
}


unsigned int PURE_FN_ATTR
pm_lcm(unsigned int const x, 
       unsigned int const y,
       unsigned int const z,
       unsigned int const limit) {
/*----------------------------------------------------------------------------
  Compute the least common multiple of 'x', 'y', and 'z'.  If it's bigger than
  'limit', though, just return 'limit'.
-----------------------------------------------------------------------------*/
    unsigned int biggest;
    unsigned int candidate;

    if (x == 0 || y == 0 || z == 0)
        pm_error("pm_lcm(): Least common multiple of zero taken.");

    biggest = MAX(x, MAX(y,z));

    candidate = biggest;
    while (((candidate % x) != 0 ||       /* not a multiple of x */
            (candidate % y) != 0 ||       /* not a multiple of y */
            (candidate % z) != 0 ) &&     /* not a multiple of z */
           candidate <= limit)
        candidate += biggest;

    if (candidate > limit) 
        candidate = limit;

    return candidate;
}



void
pm_init(const char * const progname,
        unsigned int const flags) {
/*----------------------------------------------------------------------------
   Initialize static variables that Netpbm library routines use.

   Any user of Netpbm library routines is expected to call this at the
   beginning of this program, before any other Netpbm library routines.

   A program may call this via pm_proginit() instead, though.
-----------------------------------------------------------------------------*/
    pm_setMessage(FALSE, NULL);

    pm_progname = progname;

#ifdef O_BINARY
#ifdef HAVE_SETMODE
    /* Set the stdin and stdout mode to binary.  This means nothing on Unix,
       but matters on Windows.
       
       Note that stdin and stdout aren't necessarily image files.  In
       particular, stdout is sometimes text for human consumption,
       typically printed on the terminal.  Binary mode isn't really
       appropriate for that case.  We do this setting here without
       any knowledge of how stdin and stdout are being used because it is
       easy.  But we do make an exception for the case that we know the
       file is a terminal, to get a little closer to doing the right
       thing.  
    */
    if (!isatty(0)) setmode(0,O_BINARY);  /* Standard Input */
    if (!isatty(1)) setmode(1,O_BINARY);  /* Standard Output */
#endif /* HAVE_SETMODE */
#endif /* O_BINARY */
}



static void
showVersion(void) {
    pm_message( "Using libnetpbm from Netpbm Version: %s", NETPBM_VERSION );
#if defined(COMPILE_TIME) && defined(COMPILED_BY)
    pm_message( "Compiled %s by user \"%s\"",
                COMPILE_TIME, COMPILED_BY );
#endif
#ifdef BSD
    pm_message( "BSD defined" );
#endif /*BSD*/
#ifdef SYSV
    pm_message( "SYSV defined" );
#endif /*SYSV*/
#ifdef MSDOS
    pm_message( "MSDOS defined" );
#endif /*MSDOS*/
#ifdef AMIGA
    pm_message( "AMIGA defined" );
#endif /* AMIGA */
    {
        const char * rgbdef;
        pm_message( "RGB_ENV='%s'", RGBENV );
        rgbdef = getenv(RGBENV);
        if( rgbdef )
            pm_message( "RGBENV= '%s' (env vbl set to '%s')", 
                        RGBENV, rgbdef );
        else
            pm_message( "RGBENV= '%s' (env vbl is unset)", RGBENV);
    }
}



static void
showNetpbmHelp(const char progname[]) {
/*----------------------------------------------------------------------------
  Tell the user where to get help for this program, assuming it is a Netpbm
  program (a program that comes with the Netpbm package, as opposed to a 
  program that just uses the Netpbm libraries).

  Tell him to go to the URL listed in the Netpbm configuration file.
  The Netpbm configuration file is the file named by the NETPBM_CONF
  environment variable, or /etc/netpbm if there is no such environment
  variable.

  If the configuration file doesn't exist or can't be read, or doesn't
  contain a DOCURL value, tell him to go to a hardcoded source for
  documentation.
-----------------------------------------------------------------------------*/
    const char * netpbmConfigFileName;
    FILE * netpbmConfigFile;
    char * docurl;

    if (getenv("NETPBM_CONF"))
        netpbmConfigFileName = getenv("NETPBM_CONF");
    else 
        netpbmConfigFileName = "/etc/netpbm";
    
    netpbmConfigFile = fopen(netpbmConfigFileName, "r");
    if (netpbmConfigFile == NULL) {
        pm_message("Unable to open Netpbm configuration file '%s'.  "
                   "Errno = %d (%s).  "
                   "Use the NETPBM_CONF environment variable "
                   "to control the identity of the Netpbm configuration file.",
                   netpbmConfigFileName,errno, strerror(errno));
        docurl = NULL;
    } else {
        docurl = NULL;  /* default */
        while (!feof(netpbmConfigFile) && !ferror(netpbmConfigFile)) {
            char line[80+1];
            fgets(line, sizeof(line), netpbmConfigFile);
            if (line[0] != '#') {
                sscanf(line, "docurl=%s", docurl);
            }
        }
        if (docurl == NULL)
            pm_message("No 'docurl=' line in Netpbm configuration file '%s'.",
                       netpbmConfigFileName);

        fclose(netpbmConfigFile);
    }
    if (docurl == NULL)
        pm_message("We have no reliable indication of where the Netpbm "
                   "documentation is, but try "
                   "http://netpbm.sourceforge.net or email "
                   "Bryan Henderson (bryanh@giraffe-data.com) for help.");
    else
        pm_message("This program is part of the Netpbm package.  Find "
                   "documentation for it at %s/%s\n", docurl, progname);
}



void
pm_proginit(int * const argcP, const char * argv[]) {
/*----------------------------------------------------------------------------
   Do various initialization things that all programs in the Netpbm package,
   and programs that emulate such programs, should do.

   This includes processing global options.

   This includes calling pm_init() to initialize the Netpbm libraries.
-----------------------------------------------------------------------------*/
    const char * const progname = pm_arg0toprogname(argv[0]);
        /* points to static memory in this library */
    int argn, i;
    bool showmessages;
    bool show_version;
        /* We're supposed to just show the version information, then exit the
           program.
        */
    bool show_help;
        /* We're supposed to just tell user where to get help, then exit the
           program.
        */

    pm_init(progname, 0);

    /* Check for any global args. */
    showmessages = TRUE;
    show_version = FALSE;
    show_help = FALSE;
    pm_plain_output = FALSE;
    for (argn = 1; argn < *argcP; ++argn) {
        if (pm_keymatch(argv[argn], "-quiet", 6) ||
            pm_keymatch(argv[argn], "--quiet", 7)) 
            showmessages = FALSE;
        else if (pm_keymatch(argv[argn], "-version", 8) ||
                   pm_keymatch(argv[argn], "--version", 9)) 
            show_version = TRUE;
        else if (pm_keymatch(argv[argn], "-help", 5) ||
                 pm_keymatch(argv[argn], "--help", 6) ||
                 pm_keymatch(argv[argn], "-?", 2)) 
            show_help = TRUE;
        else if (pm_keymatch(argv[argn], "-plain", 6) ||
                 pm_keymatch(argv[argn], "--plain", 7))
            pm_plain_output = TRUE;
        else
            continue;
        for (i = argn + 1; i <= *argcP; ++i)
            argv[i - 1] = argv[i];
        --(*argcP);
    }

    pm_setMessage((unsigned int) showmessages, NULL);

    if (show_version) {
        showVersion();
        exit( 0 );
    } else if (show_help) {
        pm_error("Use 'man %s' for help.", progname);
        /* If we can figure out a way to distinguish Netpbm programs from 
           other programs using the Netpbm libraries, we can do better here.
        */
        if (0)
            showNetpbmHelp(progname);
        exit(0);
    }
}


void
pm_setMessage(int   const newState,
              int * const oldStateP) {
    
    if (oldStateP)
        *oldStateP = pm_showmessages;

    pm_showmessages = !!newState;
}



int
pm_getMessage(void) {

    return pm_showmessages;
}



static void
extractAfterLastSlash(const char * const fullPath,
                      char *       const retval,
                      size_t       const retvalSize) {
    
    char * slashPos;

    /* Chop any directories off the left end */
    slashPos = strrchr(fullPath, '/');

    if (slashPos == NULL) {
        strncpy(retval, fullPath, retvalSize);
        retval[retvalSize-1] = '\0';
    } else {
        strncpy(retval, slashPos +1, retvalSize);
        retval[retvalSize-1] = '\0';
    }

    /* Chop any .exe off the right end */
    if (strlen(retval) >= 4 && strcmp(retval+strlen(retval)-4, ".exe") == 0)
        retval[strlen(retval)-4] = 0;
}



#if MSVCRT
static void
splitpath(const char * const fullPath,
          char *       const retval,
          size_t       const retvalSize) {

    _splitpath_s(fullPath, 0, 0,  0, 0,  retval, retvalSize,  0, 0);
}
#endif



char *
pm_arg0toprogname(const char arg0[]) {
/*----------------------------------------------------------------------------
   Given a value for argv[0] (a command name or file name passed to a 
   program in the standard C calling sequence), return the name of the
   Netpbm program to which it refers.

   In the most ordinary case, this is simply the argument itself.

   But if the argument contains a slash, it is the part of the argument 
   after the last slash, and if there is a .exe on it (as there is for
   DJGPP), that is removed.

   The return value is in static storage within.  It is NUL-terminated,
   but truncated at 64 characters.
-----------------------------------------------------------------------------*/
    static char retval[64+1];
#if MSVCRT
    splitpath(arg0, retval, sizeof(retval));
#else
    extractAfterLastSlash(arg0, retval, sizeof(retval));
#endif

    return(retval);
}



unsigned int
pm_randseed(void) {

    return time(NULL) ^ getpid();

}



unsigned int
pm_parse_width(const char * const arg) {
/*----------------------------------------------------------------------------
   Return the image width represented by the decimal ASCIIZ string
   'arg'.  Fail if it doesn't validly represent a width or represents
   a width that can't be conveniently used in computation.
-----------------------------------------------------------------------------*/
    unsigned int width;
    const char * error;

    pm_interpret_uint(arg, &width, &error);

    if (error) {
        pm_error("'%s' is invalid as an image width.  %s", arg, error);
        pm_strfree(error);
    } else {
        if (width > INT_MAX-10)
            pm_error("Width %u is too large for computations.", width);
        if (width == 0)
            pm_error("Width argument must be a positive number.  You "
                     "specified 0.");
    }
    return width;
}



unsigned int
pm_parse_height(const char * const arg) {
/*----------------------------------------------------------------------------
  Same as pm_parse_width(), but for height.
-----------------------------------------------------------------------------*/
    unsigned int height;
    const char * error;

    pm_interpret_uint(arg, &height, &error);

    if (error) {
        pm_error("'%s' is invalid as an image height.  %s", arg, error);
        pm_strfree(error);
    } else {
        if (height > INT_MAX-10)
            pm_error("Height %u is too large for computations.", height);
        if (height == 0)
            pm_error("Height argument must be a positive number.  You "
                     "specified 0.");
    }
    return height;
}



