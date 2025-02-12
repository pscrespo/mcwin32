/* -*- indent-width: 4; -*- */
/*
   WIN32 util implementation

        #include "../lib/utilunix.h"

            get_group
            get_user_permissions
            save_stop_handler
            my_systemv_flags
            my_system
            tilde_expand
            open_error_pipe
            close_error_pipe
            custom_canonicalize_pathname
            canonicalize_pathname
            mc_realpath
            my_build_filenamev
            my_build_filename

   Copyright (C) 2012
   The Free Software Foundation, Inc.

   Written by: Adam Young 2012 - 2018

   Portions sourced from lib/utilunix.c, see for additional information.

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#include <config.h>

#include "libw32.h"

#include <shlobj.h>                             /* SHxx */

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>                             /* struct sigaction */
#include <limits.h>                             /* INT_MAX */
#include <malloc.h>

#include <sys/stat.h>
#include <stdarg.h>
#include <errno.h>                              /* errno */
#include <string.h>
#include <ctype.h>

#include <pwd.h>
#include <grp.h>

#include "lib/global.h"
#include "lib/vfs/vfs.h"                        /* VFS_ENCODING_PREFIX */
#include "lib/strutil.h"                        /* str_move() */
#include "lib/util.h"
#include "lib/widget.h"                         /* message() */
#include "lib/vfs/xdirentry.h"
#ifdef HAVE_CHARSET
#include "lib/charsets.h"
#endif
#include "lib/utilunix.h"

#include "win32_key.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shfolder.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")

static void             set_shell (void);
static void             set_term (void);
static void             set_home (void);
static void             set_tmpdir (void);
static void             set_editor (void);
static void             set_datadir (void);
static void             set_busybox (void);

static void             my_setenv (const char *name, const char *value, int overwrite);
static void             my_setpathenv (const char *name, const char *value, int overwrite);

static void             unixpath (char *path);
static void             dospath (char *path);

static int              system_bustargs (char *cmd, const char **argv, int cnt);
static int              system_SET (int argc, const char **argv);

static DWORD WINAPI     pipe_thread (void *data);

static const char *     busybox_cmds[] = {      /* redirected commands (see vfs/sfs module) */
//
//      ar, ash, awk, base64, basename, bash, bbconfig, bunzip2, bzcat, bzip2, cal, cat, catv, chmod, cksum, clear, cmp, comm, cp, cpio, cut, date, dc, dd, df, diff,
//      dirname, dos2unix, du, echo, ed, egrep, env, expand, expr, false, fgrep, find, fold, ftpget, ftpput, getopt, grep, gunzip, gzip, hd, head, hexdump, kill, killall, ls,
//      lzcat, lzma, lzop, lzopcat, man, md5sum, mkdir, mktemp, mv, od, patch, pgrep, pidof, printenv, printf, ps, pwd, rev, rm, rmdir, sed, seq, sh, sha1sum, sha256sum, sha3sum,
//      sha512sum, shuf, sleep, sort, split, stat, strings, sum, tac, tail, tar, tee, test, touch, tr, true, uname, uncompress, unexpand, uniq, unix2dos, unlink, unlzma, unlzop,
//      unxz, unzip, usleep, uudecode, uuencode, vi, wc, wget, which, whoami, xargs, xz, xzcat, yes, zcat
//
        "ar", "ash", "awk", "base64", "bunzip2", "bzcat", "bzip2", "cat", "cksum", "cpio", "dd", "diff",
        "dos2unix", "echo", "gunzip", "gzip", "ls",
        "lzcat", "lzma", "lzop", "lzopcat", "ps",
        "tar", "uncompress", "unexpand", "unix2dos", "unlzma", "unlzop",
        "unxz", "unzip", "uudecode", "uuencode", "xz", "xzcat", "zcat"

// MISSING: lz4, ulz4
        };
static const char       bin_sh[] = "/bin/sh";
static const char       cmd_sh[] = "cmd.exe";

#define PE_BUFFER_SIZE          4096            /* pipe limit, plus terminator */

static CRITICAL_SECTION pe_guard;
static int              pe_open = -1;
static FILE *           pe_stream = NULL;
static char             pe_buffer[ PE_BUFFER_SIZE + 1024 + 1 ];


/**
 *  WIN32 initialisation ... see main.c
 */
void
WIN32_Setup(void)
{
    static int init = 0;

    if (init) return;
    ++init;

    _fmode = _O_BINARY;                         /* force binary mode */
    InitializeCriticalSection(&pe_guard);
#if defined(ENABLE_VFS)
        {   WSADATA wsaData = {0};
            WORD wVersionRequested = MAKEWORD(2,2); /* winsock2 */
            if (WSAStartup(wVersionRequested, &wsaData) != 0) {
                MessageBox(NULL, TEXT("WSAStartup failed!"), TEXT("Error"), MB_OK);
            }
        }
#endif  //ENABLE_VFS
    set_shell();
    set_term();
    set_home();
    set_editor();
    set_busybox();
    set_tmpdir();
}


static void
set_shell(void)
{
    my_setpathenv("SHELL", w32_getshell(), FALSE);
}


static void
set_term(void)
{
    my_setenv("TERM", "dos-console", FALSE);
}


static void
set_home(void)
{
    extern const char *g_get_user_config_dir(void);
    const char *cp;

    if ((cp = getenv("APPDATA")) != NULL) {
        my_setpathenv("MC_HOME", g_get_user_config_dir(), FALSE);
    }
}


static void
set_editor(void)
{
    my_setenv("EDITOR", "notepad.exe", FALSE);
    my_setenv("PAGER", "notepad.exe", FALSE);
}


/**
 *  MC_BUSYBOX setup
 *
 *      <exepath>\busybox.exe
 *      <exepath>\..\share\
 */
static void
set_busybox(void)
{
    const char *busybox = NULL;
    char buffer[MAX_PATH] = {0};

    if (NULL != getenv("MC_BUSYBOX")) return;

    if (w32_getexedir(buffer, sizeof(buffer)) > 0) {
        strncat(buffer, "/busybox.exe", sizeof(buffer));
        buffer[sizeof(buffer) - 1] = 0;
        canonicalize_pathname (buffer);
        if (0 == _access(buffer, X_OK)) {
            dospath(buffer);
            busybox = buffer;
        }
    }

    my_setpathenv("MC_BUSYBOX", (busybox ? busybox : "busybox"), TRUE);
}


/**
 *  MC_TMPDIR
 *
 *      <TMP>, <TEMP>, <TMPDIR>, <sysdir>, <userprofile>
 */
static void
set_tmpdir(void)
{
    if (NULL == getenv("MC_TMPDIR")) {
        const char *tmpdir = mc_TMPDIR();

        if (tmpdir && *tmpdir) {

            char buffer[MAX_PATH] = {0};
            struct passwd *pwd;
            struct stat st = {0};

            pwd = getpwuid (getuid ());             /* check permissions */
            snprintf (buffer, sizeof (buffer), "%s%cmc-%s", tmpdir, PATH_SEP, pwd->pw_name);
            buffer[sizeof(buffer) - 1] = 0;
            canonicalize_pathname (buffer);
            if (0 == lstat(buffer, &st)) {
                if (! S_ISDIR(st.st_mode)) {
                    tmpdir = NULL;
                }
            } else if (0 != w32_mkdir (buffer, S_IRWXU)) {
                tmpdir = NULL;
            }

            if (tmpdir) my_setpathenv("MC_TMPDIR", tmpdir, TRUE);
        }
    }
}


const char *
mc_TMPDIR(void)
{
    static char x_buffer[MAX_PATH];

    if (0 == x_buffer[0]) {
        char sysdir[MAX_PATH] = {0};
        const char *tmpdir;

        tmpdir = getenv("TMP");                     /* determine the temp directory */
        if (!tmpdir) tmpdir = getenv("TEMP");
        if (!tmpdir) tmpdir = getenv("TMPDIR");
        if (!tmpdir) {
            if (w32_getsysdir (SYSDIR_TEMP, sysdir, sizeof(sysdir)) > 0) {
                tmpdir = sysdir;
            }
        }
        if (!tmpdir) tmpdir = getenv("USERPROFILE");
        if (!tmpdir) tmpdir = TMPDIR_DEFAULT;

        strncpy(x_buffer, tmpdir, sizeof(x_buffer));
        x_buffer[sizeof(x_buffer) - 1] = 0;
        unixpath(x_buffer);
    }

    return (x_buffer[0] ? x_buffer : NULL);
}


/**
 *  Retrieve global system configuration path, equivalent to '/etc/mc'.
 *
 *      <EXEPATH>
 *          <exepath>\etc\
 *
 *      <INSTALLPATH>
 *          X:\Program Files\<Midnight Commander>\etc
 *
 *              SHGetFolderPath(CSIDL_PROGRAM_FILES)
 *              or getenv(ProgramFiles)
 *
 *      <APPDATA>
 *          X:\Documents and Settings\All Users\Application Data\<Midnight Commander>\etc\
 *
 *              SHGetFolderPath(CSIDL_COMMON_APPDATA)
 *              or getenv(ALLUSERSPROFILE)
 */
const char *
mc_SYSCONFDIR(void)
{
    static char x_buffer[MAX_PATH];

    if (0 == x_buffer[0]) {
        int len, done = FALSE;

        // <EXEPATH>, generally same as INSTALLDIR
        if ((len = w32_getexedir(x_buffer, sizeof(x_buffer))) > 0) {
            _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/etc/");
            x_buffer[sizeof(x_buffer) - 1] = 0;
            if (0 == _access(x_buffer, 0)) {
                done = TRUE;
            }
        }

        // <INSTALLPATH>
        if (! done) {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/etc/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // <APPDATA>
        if (! done)  {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/etc/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // default - INSTALLPATH
        if (! done) {
            const char *env;

            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/etc/", MC_APPLICATION_DIR);

            } else if (NULL != (env = getenv("ProgramFiles"))) {
                _snprintf(x_buffer, sizeof(x_buffer), "%s/%s/etc/", env, MC_APPLICATION_DIR);

            } else {
                _snprintf(x_buffer, sizeof(x_buffer), "c:/Program Files/%s/etc/", MC_APPLICATION_DIR);
            }
            x_buffer[sizeof(x_buffer) - 1] = 0;
            mkdir(x_buffer, S_IRWXU);
        }

        unixpath(x_buffer);
    }
    return x_buffer;
}


/**
 *  Retrieve file-name of magic database.
 *
 *      <SYSCONFDIR>\magic..
 */
const char *
mc_MAGICPATH(void)
{
    static char x_buffer[MAX_PATH];

    if (0 == x_buffer[0]) {
        _snprintf(x_buffer, sizeof(x_buffer), "%s/magic", mc_SYSCONFDIR());
        x_buffer[sizeof(x_buffer) - 1] = 0;
    }
    return x_buffer;
}


/**
 *  Retrieve global/share configuration path, equivalent to '/usr/share/mc'.
 *
 *      <EXEPATH>
 *          <exepath>\share\
 *
 *      <INSTALLPATH>
 *          X:\Program Files\<Midnight Commander>\share
 *
 *              SHGetFolderPath(CSIDL_PROGRAM_FILES)
 *              or getenv(ProgramFiles)
 *
 *      <APPDATA>
 *          X:\Documents and Settings\All Users\Application Data\<Midnight Commander>\share\
 *
 *              SHGetFolderPath(CSIDL_COMMON_APPDATA)
 *              or getenv(ALLUSERSPROFILE)
 */
const char *
mc_DATADIR(void)
{
    static char x_buffer[MAX_PATH];

    if (0 == x_buffer[0]) {
        int len, done = FALSE;

        // <EXEPATH>, generally same as INSTALLDIR
        if ((len = w32_getexedir(x_buffer, sizeof(x_buffer))) > 0) {
            _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/share/");
            x_buffer[sizeof(x_buffer) - 1] = 0;
            if (0 == _access(x_buffer, 0)) {
                done = TRUE;
            }
        }

        // <INSTALLPATH>
        if (! done) {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/share/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // <APPDATA>
        if (! done)  {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/share/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // default - INSTALLPATH
        if (! done) {
            const char *env;

            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/share/", MC_APPLICATION_DIR);

            } else if (NULL != (env = getenv("ProgramFiles"))) {
                _snprintf(x_buffer, sizeof(x_buffer), "%s/%s/share/", env, MC_APPLICATION_DIR);

            } else {
                _snprintf(x_buffer, sizeof(x_buffer), "c:/Program Files/%s/share/", MC_APPLICATION_DIR);
            }
            x_buffer[sizeof(x_buffer) - 1] = 0;
            mkdir(x_buffer, S_IRWXU);
        }

        unixpath(x_buffer);
    }
    return x_buffer;
}


/**
 *  Retrieve locale configuration path, equivalent to '/usr/share/locale'.
 *
 *      <EXEPATH>
 *          <exepath>\locale\
 *
 *      <INSTALLPATH>
 *          X:\Program Files\<Midnight Commander>\locale
 *
 *              SHGetFolderPath(CSIDL_PROGRAM_FILES)
 *              or getenv(ProgramFiles)
 *
 *      <APPDATA>
 *          X:\Documents and Settings\All Users\Application Data\<Midnight Commander>\locale\
 *
 *              SHGetFolderPath(CSIDL_COMMON_APPDATA)
 *              or getenv(ALLUSERSPROFILE)
 */
const char *
mc_LOCALEDIR(void)
{
    static char x_buffer[MAX_PATH];

    if (0 == x_buffer[0]) {
        int len, done = FALSE;

        // <EXEPATH>, generally same as INSTALLDIR
        if ((len = w32_getexedir(x_buffer, sizeof(x_buffer))) > 0) {
            _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/locale/");
            x_buffer[sizeof(x_buffer) - 1] = 0;
            if (0 == _access(x_buffer, 0)) {
                done = TRUE;
            }
        }

        // <INSTALLPATH>
        if (! done) {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/locale/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // <APPDATA>
        if (! done)  {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/locale/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // default - INSTALLPATH
        if (! done) {
            const char *env;

            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/locale/", MC_APPLICATION_DIR);

            } else if (NULL != (env = getenv("ProgramFiles"))) {
                _snprintf(x_buffer, sizeof(x_buffer), "%s/%s/locale/", env, MC_APPLICATION_DIR);

            } else {
                _snprintf(x_buffer, sizeof(x_buffer), "c:/Program Files/%s/locale/", MC_APPLICATION_DIR);
            }
            x_buffer[sizeof(x_buffer) - 1] = 0;
            mkdir(x_buffer, S_IRWXU);
        }

        unixpath(x_buffer);
    }
    return x_buffer;
}


/**
 *  Retrieve global/share plugin configuration path, equivalent to '/lib/mc'.
 *
 *      <EXEPATH>
 *          <exepath>\etc\
 *
 *      <INSTALLPATH>
 *          X:\Program Files\<Midnight Commander>\etc
 *
 *              SHGetFolderPath(CSIDL_PROGRAM_FILES)
 *              or getenv(ProgramFiles)
 *
 *      <APPDATA>
 *          X:\Documents and Settings\All Users\Application Data\<Midnight Commander>\etc\
 *
 *              SHGetFolderPath(CSIDL_COMMON_APPDATA)
 *              or getenv(ALLUSERSPROFILE)
 */
const char *
mc_LIBEXECDIR(void)
{
    static char x_buffer[MAX_PATH];

    if (0 == x_buffer[0]) {
        int len, done = FALSE;

        // <EXEPATH>, generally same as INSTALLDIR
        if ((len = w32_getexedir(x_buffer, sizeof(x_buffer))) > 0) {
            _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/plugin/");
            x_buffer[sizeof(x_buffer) - 1] = 0;
            if (0 == _access(x_buffer, 0)) {
                done = TRUE;
            }
        }

        // <INSTALLPATH>
        if (! done) {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/plugin/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // <APPDATA>
        if (! done)  {
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/plugin/", MC_APPLICATION_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    done = TRUE;
                }
            }
        }

        // default - INSTALLPATH
        if (! done) {
            const char *env;

            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, x_buffer))) {
                len = strlen(x_buffer);
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/plugin/", MC_APPLICATION_DIR);

            } else if (NULL != (env = getenv("ProgramFiles"))) {
                _snprintf(x_buffer, sizeof(x_buffer), "%s/%s/plugin/", env, MC_APPLICATION_DIR);

            } else {
                _snprintf(x_buffer, sizeof(x_buffer), "c:/Program Files/%s/plugin/", MC_APPLICATION_DIR);
            }
            x_buffer[sizeof(x_buffer) - 1] = 0;
            mkdir(x_buffer, S_IRWXU);
        }

        unixpath(x_buffer);
    }
    return x_buffer;
}


/**
 *  Retrieve global/share plugin configuration path, equivalent to '/lib/mc'.
 *
 *      <EXEPATH>
 *          <exepath>\etc\
 *
 *      <INSTALLPATH>
 *          X:\Program Files\<Midnight Commander>\etc
 *
 *              SHGetFolderPath(CSIDL_PROGRAM_FILES)
 *              or getenv(ProgramFiles)
 *
 *      <APPDATA>
 *          X:\Documents and Settings\All Users\Application Data\<Midnight Commander>\etc\
 *
 *              SHGetFolderPath(CSIDL_COMMON_APPDATA)
 *              or getenv(ALLUSERSPROFILE)
 */
const char *
mc_EXTHELPERSDIR(void)
{
    return mc_LIBEXECDIR();                     // one and the same ....
}


/**
 *  Retrieve the user specific configuration path.
 *
 *      <SYSCONFDIR>
 *          x:\Documents and Settings\<user>\Application Data\<Midnight Commander>\
 *
 *              SHGetFolderPath(CSIDL_APPDATA)
 *              APPDATA
 *
 *      HOME
 *          x:\<home>\<Midnight Commander>\
 *
 *      CWD
 *          <cwd><Midnight Commander>\
 */
char *
mc_USERCONFIGDIR(const char *subdir)
{
    static char x_buffer[MAX_PATH];

    if (0 == x_buffer[0]) {
        const char *env;
        int len, done = TRUE;

        // <PERSONAL>
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, x_buffer)) &&
                            (len = strlen(x_buffer)) > 0) {
                                                /* personal settings */
            _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/", MC_USERCONF_DIR);
            x_buffer[sizeof(x_buffer) - 1] = 0;
            if (0 == _access(x_buffer, 0)) {
                x_buffer[len+1] = 0;
                done = TRUE;
            }
        }

        // <APPDATA>
        if (! done) {
            if ((env = getenv("APPDATA")) != NULL && (len = strlen(env)) > 0) {
                                                /* personal settings */
                _snprintf(x_buffer, sizeof(x_buffer), "%s/%s/", env, MC_USERCONF_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    x_buffer[len+1] = 0;
                    done = TRUE;
                }
            }
        }

        // <HOME>
        if (! done) {
            if ((env = getenv("HOME")) != NULL && (len = strlen(env)) > 0) {
                                                /* personal settings, new */
                _snprintf(x_buffer, sizeof(x_buffer), "%s/%s/", env, MC_USERCONF_DIR);
                x_buffer[sizeof(x_buffer) - 1] = 0;
                if (0 == _access(x_buffer, 0)) {
                    x_buffer[len+1] = 0;
                    done = TRUE;
                }
            }
        }

        // new user
        if (! done) {
            const char *env;
                                                /* personal settings */
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, x_buffer)) &&
                                (len = strlen(x_buffer)) > 0) {
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/", MC_USERCONF_DIR);
                done = TRUE;
                                                /* old school configuration */
            } else if ((env = getenv("HOME")) != NULL && (len = strlen(env)) > 0) {
                _snprintf(x_buffer, sizeof(x_buffer), "%s/%s/", env, MC_USERCONF_DIR);
                done = TRUE;
                                                /* full back */
            } else if (w32_getcwd(x_buffer, sizeof(x_buffer)) && (len = strlen(x_buffer)) > 0) {
                _snprintf(x_buffer + len, sizeof(x_buffer) - len, "/%s/", MC_USERCONF_DIR);
                done = TRUE;

            }

            if (! done) {
                strcpy(x_buffer, "./");         /* !! */
            } else {
                x_buffer[sizeof(x_buffer) - 1] = 0;
                mkdir(x_buffer, S_IRWXU);
                x_buffer[len+1] = 0;            /* remove trailing subdirectory, leave seperator */
            }
        }

        unixpath(x_buffer);
    }

    if (subdir && *subdir) {
        const int dirlen = strlen(x_buffer) + strlen(subdir) + 2;
        char *dir = g_malloc(dirlen);

        _snprintf(dir, dirlen, "%s%s/", x_buffer, subdir);
        if (-1 == _access(dir, 0)) {
            w32_mkdir(dir, 0666);
        }
        return dir;
    }

    return g_strdup(x_buffer);
}


static void
my_setenv(const char *name, const char *value, int overwrite)
{
    if ((1 == overwrite) || NULL == getenv(name)) {
#if defined(__WATCOMC__)
        setenv(name, value, TRUE);
#else
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s=%s", name, value);
        buf[sizeof(buf)-1] = 0;
        putenv(strdup(buf));
#endif
    }
}


static void
my_setpathenv(const char *name, const char *value, int overwrite)
{
    char buf[1024];

    if ((1 == overwrite) || NULL == getenv(name)) {
#if defined(__WATCOMC__)
        strncpy(buf, value, sizeof(buf));
        buf[sizeof(buf)-1] = 0;
        canonicalize_pathname(buf);
        dospath(buf);
        setenv(name, (const char *)buf, TRUE);
#else
        snprintf(buf, sizeof(buf), "%s=%s", name, value);
        canonicalize_pathname(buf + strlen(name) + 1);
        buf[sizeof(buf) - 1] = 0;
        putenv(buf);
#endif
    }
}


int
WIN32_checkheap(void)
{
    int rc = 0;

    switch (_heapchk()) {
    case _HEAPOK:
    case _HEAPEMPTY:
        break;
    case _HEAPBADBEGIN:
        printf("ERROR - heap is damaged\n");
        rc = -1;
        break;
    case _HEAPBADNODE:
        printf("ERROR - bad node in heap\n");
        rc = -1;
        break;
    }
    return (rc);
}


const char *
get_owner(uid_t uid)
{
    static char ubuf [10];
    struct passwd *pwd;

    pwd = getpwuid (uid);
    if (pwd) {
        return (char *)pwd->pw_name;
    }
    if (uid == 0) {
        return (char *)"root";
    }
    _snprintf (ubuf, sizeof (ubuf), "%d", uid);
    return ubuf;

}


const char *
get_group(gid_t gid)
{
    static char gbuf [10];
    struct group *grp;

    grp = getgrgid (gid);
    if (grp) {
        return (char *)grp->gr_name;
    }
    if (gid == 0) {
        return (char *)"root";
    }
    _snprintf (gbuf, sizeof (gbuf), "%d", gid);
    return gbuf;

}


int
get_user_permissions(struct stat *st)
{
    static gboolean initialized = FALSE;
    static gid_t *groups;
    static int ngroups;
    static uid_t uid;
    int i;

    if (!initialized) {
        uid = geteuid ();

        ngroups = getgroups (0, NULL);
        if (-1 == ngroups) {
            ngroups = 0;                        /* ignore errors */
        }


        /* allocate space for one element in addition to what
         * will be filled by getgroups(). */
        groups = g_new (gid_t, ngroups + 1);

        if (ngroups != 0) {
            ngroups = getgroups (ngroups, groups);
            if (-1 == ngroups) {
                ngroups = 0;                    /* ignore errors */
            }
        }

        /* getgroups() may or may not return the effective group ID,
         * so we always include it at the end of the list. */
        groups[ngroups++] = w32_getegid ();

        initialized = TRUE;
    }

    if (st->st_uid == uid || 0 == uid) {
        return 0;
    }

    for (i = 0; i < ngroups; ++i) {
        if (st->st_gid == groups[i]) {
            return 1;
        }
    }

    return 2;
}


void
save_stop_handler(void)
{
}


/**
 * Call external programs with flags and with array of strings as parameters.
 *
 * @parameter flags   addition conditions for running external programs.
 * @parameter command shell (if flags contain EXECUTE_AS_SHELL), command to run otherwise.
 *                    Shell (or command) will be found in paths described in PATH variable
 *                    (if shell parameter doesn't begin from path delimiter)
 * @parameter argv    Array of strings (NULL-terminated) with parameters for command
 * @return 0 if successfull, -1 otherwise
 */

int
my_systemv_flags (int flags, const char *command, char *const argv[])
{
    char *cmd = 0;
    int status = 0;

    if (argv) {
        const char *str;
        unsigned idx, slen = 0;
        char *cursor;

        for (idx = 0; NULL != (str = argv[idx]); ++idx)
            if (*str) {
                slen += strlen(str) + 1;
            }

        cursor = cmd = g_malloc(slen);

        for (idx = 0; NULL != (str = argv[idx]); ++idx)
            if (*str) {
                slen = strlen(str);
                if (cursor != cmd) *cursor++ = ' ';
                memcpy(cursor, argv[idx], slen);
                cursor += slen;
            }

        *cursor = '\0';
    }
    status = my_system (flags, command, (cmd ? cmd : ""));
    g_free (cmd);
    return status;
}


/**
 * Call external programs.
 *
 * @parameter flags   addition conditions for running external programs.
 * @parameter shell   shell (if flags contain EXECUTE_AS_SHELL), command to run otherwise.
 *                    Shell (or command) will be found in paths described in PATH variable
 *                    (if shell parameter doesn't begin from path delimiter)
 * @parameter command Command for shell (or first parameter for command, if flags contain EXECUTE_AS_SHELL)
 * @return 0 if successfull, -1 otherwise
 */
int
my_system(int flags, const char *shell, const char *cmd)
{
    const char *busybox = getenv("MC_BUSYBOX");
    int shelllen, ret = -1;

    if ((flags & EXECUTE_INTERNAL) && cmd) {
        printf("%s\n", cmd);                    /* echo command */
    }

    if (cmd) {
        while (' ' == *cmd) ++cmd;              /* consume leading whitespace (if any) */
            /*whitespace within "#! xxx" shall be visible; confusing matching logic below */
    }

    if (busybox && *busybox) {
        if (shell && 0 == strncmp(shell, bin_sh, sizeof(bin_sh)-1)) {
            /*
             *  If <shell> = </bin/sh> <cmd ...>
             *  execute as <busybox cmd ...>
             */
            const char *space;

            if (NULL != (space = strchr(cmd, ' '))) {
                const int cmdlen = space - cmd;
                unsigned i;

                for (i = 0; i < (sizeof(busybox_cmds)/sizeof(busybox_cmds[0])); ++i)  {
                    if (0 == strncmp(busybox_cmds[i], cmd, cmdlen)) {
                        char *t_cmd;

                        if (NULL != (t_cmd = g_strconcat("\"", busybox, "\" ", cmd, NULL))) {
                            key_shell_mode();
                            ret = w32_shell(NULL, t_cmd, NULL, NULL, NULL);
                            key_prog_mode();
                            g_free(t_cmd);
                        }
                        return ret;
                    }
                }
            }

        } else if (cmd) {
            /*
             *  If <cmd> </bin/sh ...>
             *  execute as <shell> <busybox sh ...>
             */
            const char *space;

            if (NULL != (space = strchr(cmd, ' ')) &&
                    space == (cmd + (sizeof(bin_sh) - 1)) && 0 == strncmp(cmd, bin_sh, sizeof(bin_sh)-1)) {
                char *t_cmd;

                if (NULL != (t_cmd = g_strconcat("\"", busybox, "\" sh", space, NULL))) {
                    key_shell_mode();
                    ret = w32_shell(shell, t_cmd, NULL, NULL, NULL);
                    key_prog_mode();
                    g_free(t_cmd);
                }
                return ret;
            }
        }
    }

    if ((flags & EXECUTE_AS_SHELL) && cmd) {    /* internal commands */
    #define MAX_ARGV    128
        const char *argv[MAX_ARGV + 1];
        char cbuf[4 * 1048];
        int argc;

        (void) strncpy(cbuf, cmd, sizeof(cbuf));
        if (strlen(cmd) < sizeof(cbuf) &&
                (argc = system_bustargs(cbuf, argv, MAX_ARGV)) <= MAX_ARGV && argc > 0) {

            if (0 == strcmp(argv[0], "set")) {
                return system_SET(argc, argv);

        //  } else if (0 == strcmp(argv[0], "echo")) {
        //      return system_SET(argc, argv);
        //
            }
            /*XXX - others? */
        }
    }

    /*
     *  If <cmd.exe> < ...>
     *  convert any / to \ in first word
     */
    shelllen = (shell ? strlen(shell) : 0);
    if ((shelllen -= (sizeof(cmd_sh)-1)) >= 0 &&
            0 == _strnicmp(shell + shelllen, cmd_sh, sizeof(cmd_sh)-1)) {
        char *t_cmd, *cursor;

        if (NULL != (t_cmd = strdup(cmd))) {
            for (cursor = t_cmd; *cursor && *cursor != ' '; ++cursor) {
                if ('/' == *cursor) *cursor = '\\';
            }
            key_shell_mode();
            ret = w32_shell(shell, t_cmd, NULL, NULL, NULL);
            key_prog_mode();
            free(t_cmd);
            return ret;
        }
    }

    key_shell_mode();
    ret = w32_shell(shell, cmd, NULL, NULL, NULL);
    key_prog_mode();
    return ret;
}


/**
 *  'set' command replacement
 */
static int
system_SET(int argc, const char **argv)
{
    if (argc == 1) {
        extern char **environ;                  /* MSVC/WATCOM */
        char **env = environ;

        if (env) {
            while (*env) {
                printf( "%s\n", *env);
                ++env;
            }
        }
    } else {
        char *p;

        if ((p = strchr(argv[1], '=')) != NULL) {
            *p++ = '\0';
            my_setenv(argv[1], p, 1);
        } else {
            if ((p = getenv(argv[1])) == NULL) {
                printf("Environment variable %s not defined\n", argv[1]);
            } else {
                printf("%s=%s\n", argv[1], p );
            }
        }
    }
    return (0);
}


/**
 *  Determine if a #! script and return underlying exec handler.
 *  TODO: Return resolved path to perl, python etc (utilise file/extension association)
 */
static const char *
IsScript(const char *cmd)
{
    char t_cmd[1024] = { 0 }, magic[128] = { 0 };
    const char *script = NULL;
    const char *argv[3] = { 0 };
    int fd;

    strncpy(t_cmd, cmd, sizeof(t_cmd)-1);
    if (system_bustargs(t_cmd, argv, 2) >= 1) {
        if ((fd = _open(argv[0], O_RDONLY | O_BINARY)) >= 0) {
            if (_read(fd, magic, sizeof(magic) - 1) > 2 && magic[0] == '#' && magic[1] == '!') {
                // sha-bang
                const char *exec = magic + 2;
                int len = -1;

                while (*exec && ' ' == *exec) ++exec;
                if (*exec == '/') {
                        if (0 == strncmp(exec, "/bin/sh", len = (sizeof("/bin/sh")-1)))
                                script = "sh";
                        else if (0 == strncmp(exec, "/bin/ash", len = (sizeof("/bin/ash")-1)))
                                script = "ash";
                        else if (0 == strncmp(exec, "/bin/bash", len = (sizeof("/bin/bash")-1)))
                                script = "bash";
                        else if (0 == strncmp(exec, "/bin/sed", len = (sizeof("/bin/sed")-1)))
                                script = "sed";
                        else if (0 == strncmp(exec, "/bin/awk", len = (sizeof("/bin/awk")-1)))
                                script = "awk";
                        else if (0 == strncmp(exec, "/usr/bin/perl", len = (sizeof("/usr/bin/perl")-1)))
                                script = "perl";
                        else if (0 == strncmp(exec, "/usr/bin/python", len = (sizeof("/usr/bin/python")-1)))
                                script = "python";
                        //else, ignore others
                        if (script && exec[len] != ' ' && exec[len] != '\n' && exec[len] != '\r') {
                            script = NULL;      //bad termination, ignore
                        }
                }
            }
            _close(fd);
        }
    }
    return script;
}


/**
 *  popen() implementation
 */
FILE *
win32_popen(const char *cmd, const char *mode)
{
    const char *busybox = getenv("MC_BUSYBOX");
    const char *space, *exec;
    FILE *file = NULL;

    if (busybox && *busybox &&
            NULL != (space = strchr(cmd, ' ')) &&
                space == (cmd + (sizeof(bin_sh) - 1)) && 0 == strncmp(cmd, bin_sh, sizeof(bin_sh) - 1)) {
        /*
         *  If <cmd> </bin/sh ...>
         *  execute as <shell> <busybox sh ...>
         */
        char *t_cmd;

        if (NULL != (t_cmd = g_strconcat("\"", busybox, "\" sh", space, NULL))) {
            file = w32_popen(t_cmd, mode);
            g_free(t_cmd);
        }
    } else if (busybox && *busybox && NULL != (exec = IsScript(cmd))) {
        /*
         *  If <#!> </bin/sh | /usr/bin/perl | /usr/bin/python>
         *      note: currently limited to extfs usage.
         */
        char *t_cmd = NULL;

        if (exec[0] == 'p') {                   // perl/python
                t_cmd = g_strconcat(exec, " ", cmd, NULL);
        } else {                                // sh/ash/bash
                t_cmd = g_strconcat("\"", busybox, "\" ", exec, " ", cmd, NULL);
        }
        if (t_cmd) {
            file = w32_popen(t_cmd, mode);
            g_free(t_cmd);
        }
    } else {
        file = w32_popen(cmd, mode);
    }

    if (pe_open >= 0) {
        if (NULL == file) {
            pe_open += _snprintf(pe_buffer, PE_BUFFER_SIZE, "popen ; %s", strerror(errno));

        } else {
            HANDLE hThread;

            pe_stream = file;
            if (0 != (hThread = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL))) {
                SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
                CloseHandle(hThread);
                sleep(10);                      /* yield */
            }
        }
    }
    return file;
}


/**
 *  pclose() implementation
 */
int
win32_pclose(FILE *file)
{
    EnterCriticalSection(&pe_guard);
    pe_stream = NULL;
    LeaveCriticalSection(&pe_guard);
    return w32_pclose(file);
}


/**
 *  Creates a pipe to hold standard error for a later analysis.  The pipe
 *  can hold 4096 bytes. Make sure no more is written or a deadlock
 *  might occur.
 *
 *  Returns true if an error was displayed
 *  error: -1 - ignore errors, 0 - display warning, 1 - display error
 *  text is prepended to the error message from the pipe
 */
void
open_error_pipe(void)
{
    pe_open = 0;                                // open stream
}


int
close_error_pipe(int error, const char *text)
{
    const char *title;
    int len;

    EnterCriticalSection(&pe_guard);
    len = pe_open;
    pe_open = -1;
    pe_stream = NULL;
    LeaveCriticalSection(&pe_guard);
    if (len < 0) {
        return 0;
    }

    if (error < 0 || (error > 0 && (error & D_ERROR) != 0)) {
        title = MSG_ERROR;
    } else {
        title = _("Warning");
    }

    if (error < 0) {
        return 0;                               /* just ignore error message */
    }

    /* Show message from pipe */
    if (text == NULL) {
        if (len <= 0) {
            return 0;                           /* Nothing to show */
        }
        pe_buffer[len] = 0;
        text = pe_buffer;

    } else {                                    /* Show given text and possible message from pipe */
        const size_t textlen = strlen(text);

        if (textlen + len < sizeof(pe_buffer)) {
            memmove(pe_buffer + textlen + 1, (const char *)pe_buffer, len);
            memmove(pe_buffer, text, textlen);
            len  += textlen + 1;
            pe_buffer[textlen] = '\n';
            pe_buffer[len] = 0;
            text = pe_buffer;
        }
    }

    query_dialog(title, text, D_NORMAL, 1, _("&Ok"));
//  message(error, title, "%s", text);
    return 1;
}


/**
 *  popen stderr consumer
 */
static DWORD WINAPI
pipe_thread(void *data)
{
    FILE *file;
    char buffer[ PE_BUFFER_SIZE ];

    EnterCriticalSection(&pe_guard);
    if (pe_open >= 0 && NULL != (file = pe_stream)) {
        while (1) {
            int len;

            LeaveCriticalSection(&pe_guard);    /* consume stderr */
            len = w32_pread_err(pe_stream, buffer, sizeof(buffer));
            EnterCriticalSection(&pe_guard);

            if (len >= 0 && pe_open >= 0 && file == pe_stream) {
                if (0 == len) {
                    if (GetLastError() == ERROR_BROKEN_PIPE) {
                        break;                  /* pipe done */
                    }
                } else {
                    int newlen = 0;
                    const char *peend, *cursor = buffer;
                    char *pe = pe_buffer;

                    peend = pe + PE_BUFFER_SIZE, pe += pe_open;
                    while (len-- > 0 && pe < peend) {
                        if ('\r' != *cursor) {
                            *pe++ = *cursor;
                            ++newlen;
                        }
                        ++cursor;
                    }
                    pe_open += newlen;
                }
                continue;
            }
            break;                              /* stream invalid */
        }
    }
    LeaveCriticalSection(&pe_guard);
    return 0;
}


static void
unixpath(char *path)
{
    const char *in = path;

    while (*in) {
        if ('/' == *in || '\\' == *in) {
            ++in;
            while ('/' == *in || '\\' == *in) {
                ++in;
            }
            *path++ = PATH_SEP;
        } else {
            *path++ = *in++;
        }
    }
    *path = 0;
}


static void
dospath(char *path)
{
    const char *in = path;

    while (*in) {
        if ('/' == *in || '\\' == *in) {
            ++in;
            while ('/' == *in || '\\' == *in) {
                ++in;
            }
            *path++ = '\\';
        } else {
            *path++ = *in++;
        }
    }
    *path = 0;
}


/**
 *  Splits 'cmd' into list of argument pointers.
 */
static int
system_bustargs(char *cmd, const char **argv, int cnt)
{
    char *start, *end;
    int argc;

    --cnt;                                      /* nul terminator storage */
    for (argc = 0;;) {
        /* Skip over blanks */
        while (*cmd == ' '|| *cmd == '\t' || *cmd == '\n') {
            ++cmd;                              /* eat white space */
        }
        if (*cmd == '\0') {                     /* termination */
             break;
        }

        /* Retrieve argument */
        if (*cmd == '\"') {                     /* quoted argument */
            ++cmd;
            start = end = cmd;
            for (;;) {
                if (*cmd == '\n' || *cmd == '\0')
                    break;
                if (*cmd == '\"')
                    break;
                if (*cmd == '\\') {
                    if (cmd[1] == '\"' || cmd[1] == '\\') {
                        ++cmd;
                    }
                }
                *end++ = *cmd++;
             }
        } else {
            start = end = cmd;
            for (;;) {
                if (*cmd == '\n' || *cmd == '\0')
                    break;
                if (*cmd == ' '  || *cmd == '\t')
                    break;
                if (*cmd == '\\' && cmd[1] == '\"')
                    ++cmd;
                *end++ = *cmd++;
            }
        }

        /* reallocate argument list index */
        if (cnt > 0) {
            argv[ argc++ ] = start;
            if (*cmd == '\0')
                break;
            *end = '\0';
            ++cmd;
            --cnt;
        }
    }
    argv[ argc ] = NULL;
    return argc;
}


/**
 *  Directory references expansion ..
 */
char *
tilde_expand(const char *directory)
{
    if (PATH_SEP == directory[0] &&             /* fix '/X:', vfs work around */
            isalpha((unsigned char)directory[1]) && ':' == directory[2]) {
        ++directory;
    }

    if (PATH_SEP == *directory) {               /* / ==> x:/ */

        if (PATH_SEP != directory[1] ||         /* preserve URL's (//<server) */
                    0 == directory[2] || PATH_SEP == directory[2]) {
            const char *cwd = vfs_get_current_dir ();

            if (':' == cwd[1]) {
                char drive[3] =  "x:";
                drive[0] = toupper (cwd[0]);
                return g_strconcat (drive, directory, NULL);
            }
        }

    } else if ('.' == *directory && 0 == directory[1]) {

        char *cwd = vfs_get_current_dir_n ();
        if (cwd) {                              /* . ==> <cwd> */
            return cwd;
        }

    } else if ('~' == *directory) {

        struct passwd *passwd = NULL;
        const char *home = NULL, *p, *q;

        p = directory + 1;
        q = strchr (p, PATH_SEP);

        if (!(*p) || (*p == PATH_SEP)) {        /* d = "~" or d = "~/" */
            passwd = getpwuid (geteuid ());
            if (passwd) home = passwd->pw_dir;
            if (NULL == home || !*home) home = getenv("HOME");
            q = (*p == PATH_SEP) ? p + 1 : "";

        } else {
            if (!q) {
                passwd = getpwnam (p);
            } else {
                char *name;

                if (NULL != (name = g_malloc (q - p + 1))) {
                    strncpy (name, p, q - p);
                    name[q - p] = 0;
                    passwd = getpwnam (name);
                    g_free (name);
                }
            }
            if (passwd) home = passwd->pw_dir;
        }

        if (home && *home) {
            return g_strconcat (home, PATH_SEP_STR, q, NULL);
        }
    }

    return g_strdup (directory);
}


/**
 *  Canonicalize path, and return a new path.  Do everything in place.
 *
 *  The new path differs from path in:
 *      Multiple `/'s are collapsed to a single `/'.
 *      Leading  `./'s and trailing `/.'s are removed.
 *      Trailing `/'s are removed.
 *      Non-leading `../'s and trailing `..'s are handled by removing
 *      portions of the path.
 *
 *  Well formed UNC paths are modified only in the local part.
 *
 *  Notes: Sourced from lib/utilunix.c
 */
void
custom_canonicalize_pathname(char *orgpath, CANON_PATH_FLAGS flags)
{
    const size_t url_delim_len = strlen (VFS_PATH_URL_DELIMITER);
    char *lpath = orgpath;                      /* path without leading UNC part */
    int unc = FALSE;
    char *p, *s;
    size_t len;

    /* Standardise to the system seperator */
    for (s = lpath; *s; ++s) {
        if ('\\' == *s || '/' == *s) {
            *s = PATH_SEP;
        }
    }

    /* Detect and preserve UNC paths: //server/... */
    if ((flags & CANON_PATH_GUARDUNC) &&
                lpath[0] == PATH_SEP && lpath[1] == PATH_SEP && lpath[2]) {
        p = lpath + 2;
        while (p[0] && p[0] != '/') {
            ++p;
        }
        if (p[0] == '/' && p > orgpath + 2) {
            lpath = p;
            unc = TRUE;
        }
    }

    if (!lpath[0] || !lpath[1])
        return;

    /* DOS'ish
     *  o standardize seperator
     *  o preserve leading drive
     */
    if (!unc) {
        if (PATH_SEP == lpath[0] &&
                ':' == lpath[2] && isalpha((unsigned char)lpath[1])) {
            str_move (lpath, lpath + 1);        /* /X:, remove leading '/' vfs name mangling */
            lpath[0] = toupper(lpath[0]);
            lpath += 2;

        } else if (':' == lpath[1] && isalpha((unsigned char)lpath[0])) {
            lpath[0] = toupper(lpath[0]);
            lpath += 2;                         /* skip drive */
        }
    }

    /* Execute based on specified flags */
    if (flags & CANON_PATH_JOINSLASHES)
    {
        /* Collapse multiple slashes */
        p = lpath;
        while (*p)
        {
            if (p[0] == PATH_SEP && p[1] == PATH_SEP && (p == lpath || *(p - 1) != ':'))
            {
                s = p + 1;
                while (*(++s) == PATH_SEP);
                str_move (p + 1, s);
            }
            p++;
        }
    }

    if (flags & CANON_PATH_JOINSLASHES)
    {
        /* Collapse "/./" -> "/" */
        p = lpath;
        while (*p)
        {
            if (p[0] == PATH_SEP && p[1] == '.' && p[2] == PATH_SEP)
                str_move (p, p + 2);
            else
                p++;
        }
    }

    if (flags & CANON_PATH_REMSLASHDOTS)
    {
        /* Remove trailing slashes */
        p = lpath + strlen (lpath) - 1;
        while (p > lpath && *p == PATH_SEP)
        {
            if (p >= lpath - (url_delim_len + 1)
                    && strncmp (p - url_delim_len + 1, VFS_PATH_URL_DELIMITER, url_delim_len) == 0)
                break;
            *p-- = 0;
        }

        /* Remove leading "./" */
        if (lpath[0] == '.' && lpath[1] == PATH_SEP)
        {
            if (lpath[2] == 0)
            {
                lpath[1] = 0;
                return;
            }
            else
            {
                str_move (lpath, lpath + 2);
            }
        }

        /* Remove trailing "/" or "/." */
        len = strlen (lpath);
        if (len < 2)
            return;
        if (lpath[len - 1] == PATH_SEP
            && (len < url_delim_len
                || strncmp (lpath + len - url_delim_len, VFS_PATH_URL_DELIMITER, url_delim_len) != 0))
        {
            lpath[len - 1] = '\0';
        }
        else
        {
            if (lpath[len - 1] == '.' && lpath[len - 2] == PATH_SEP)
            {
                if (len == 2)
                {
                    lpath[1] = '\0';
                    return;
                }
                else
                {
                    lpath[len - 2] = '\0';
                }
            }
        }
    }

    if (flags & CANON_PATH_REMDOUBLEDOTS)
    {
#ifdef HAVE_CHARSET
        const size_t enc_prefix_len = strlen (VFS_ENCODING_PREFIX);
#endif /* HAVE_CHARSET */

        /* Collapse "/.." with the previous part of path */
        p = lpath;
        while (p[0] && p[1] && p[2])
        {
            if ((p[0] != PATH_SEP || p[1] != '.' || p[2] != '.') || (p[3] != PATH_SEP && p[3] != 0))
            {
                p++;
                continue;
            }

            /* search for the previous token */
            s = p - 1;
            if (s >= lpath + url_delim_len - 2
                    && strncmp (s - url_delim_len + 2, VFS_PATH_URL_DELIMITER, url_delim_len) == 0)
            {
                s -= (url_delim_len - 2);
                while (s >= lpath && *s-- != PATH_SEP);
            }

            while (s >= lpath)
            {
                if (s - url_delim_len > lpath
                        && strncmp (s - url_delim_len, VFS_PATH_URL_DELIMITER, url_delim_len) == 0)
                {
                    char *vfs_prefix = s - url_delim_len;
                    struct vfs_class *vclass;

                    while (vfs_prefix > lpath && *--vfs_prefix != PATH_SEP);
                    if (*vfs_prefix == PATH_SEP)
                        vfs_prefix++;

                    *(s - url_delim_len) = '\0';
                    vclass = vfs_prefix_to_class (vfs_prefix);
                    *(s - url_delim_len) = *VFS_PATH_URL_DELIMITER;

                    if (vclass != NULL)
                    {
                        struct vfs_s_subclass *sub = (struct vfs_s_subclass *) vclass->data;
                        if (sub != NULL && sub->flags & VFS_S_REMOTE)
                        {
                            s = vfs_prefix;
                            continue;
                        }
                    }
                }

                if (*s == PATH_SEP)
                    break;

                s--;
            }

            s++;

            /* If the previous token is "..", we cannot collapse it */
            if (s[0] == '.' && s[1] == '.' && s + 2 == p)
            {
                p += 3;
                continue;
            }

            if (p[3] != 0)
            {
                if (s == lpath && *s == PATH_SEP)
                {
                    /* "/../foo" -> "/foo" */
                    str_move (s + 1, p + 4);
                }
                else
                {
                    /* "token/../foo" -> "foo" */
#ifdef HAVE_CHARSET
                    if ((strncmp (s, VFS_ENCODING_PREFIX, enc_prefix_len) == 0)
                            && (is_supported_encoding (s + enc_prefix_len)))
                        /* special case: remove encoding */
                        str_move (s, p + 1);
                    else
#endif /* HAVE_CHARSET */
                        str_move (s, p + 4);
                }
                p = (s > lpath) ? s - 1 : s;
                continue;
            }

            /* trailing ".." */
            if (s == lpath)
            {
                /* "token/.." -> "." */
                if (lpath[0] != PATH_SEP)
                {
                    lpath[0] = '.';
                }
                lpath[1] = 0;
            }
            else
            {
                /* "foo/token/.." -> "foo" */
                if (s == lpath + 1)
                    s[0] = 0;
#ifdef HAVE_CHARSET
                else if ((strncmp (s, VFS_ENCODING_PREFIX, enc_prefix_len) == 0)
                            && (is_supported_encoding (s + enc_prefix_len)))
                {
                    /* special case: remove encoding */
                    s[0] = '.';
                    s[1] = '.';
                    s[2] = '\0';

                    /* search for the previous token */
                    /* s[-1] == PATH_SEP */
                    p = s - 1;
                    while (p >= lpath && *p != PATH_SEP)
                        p--;

                    if (p != NULL)
                        continue;
                }
#endif /* HAVE_CHARSET */
                else
                {
                    if (s >= lpath + url_delim_len
                            && strncmp (s - url_delim_len, VFS_PATH_URL_DELIMITER, url_delim_len) == 0)
                        *s = '\0';
                    else
                        s[-1] = '\0';
                }
                break;
            }

            break;
        }
    }
}


void
canonicalize_pathname(char *path)
{
    custom_canonicalize_pathname (path, CANON_PATH_ALL);
}


/**
 *  realpath() implementation.
 */
char *
mc_realpath(const char *path, char *resolved_path)
{
    if (NULL == _fullpath(resolved_path, path, MAX_PATH)) {
        strcpy(resolved_path, path);
    }
    unixpath(resolved_path);
    return resolved_path;
}



/**
 *  Build filename from arguments.
 *  Like to g_build_filename(), but respect VFS_PATH_URL_DELIMITER
 */
char *
mc_build_filenamev(const char *first_element, va_list args)
{
    gboolean absolute;
    const char *element = first_element;
    GString *path;
    char *ret;

    if (element == NULL)
        return NULL;

    path = g_string_new ("");
    absolute =
        (*first_element != '\0' && *first_element == PATH_SEP);

    do {
        if (*element == '\0') {
            element = va_arg (args, char *);
        } else {
            char *tmp_element;
            size_t len;
            const char *start;

            tmp_element = g_strdup (element);

            element = va_arg (args, char *);

            canonicalize_pathname (tmp_element);
            len = strlen (tmp_element);
            start = (tmp_element[0] == PATH_SEP) ? tmp_element + 1 : tmp_element;

            g_string_append (path, start);
            if (tmp_element[len - 1] != PATH_SEP && element != NULL) {
                g_string_append_c (path, PATH_SEP);
            }
            g_free (tmp_element);
        }
    }
    while (element != NULL);

    //WIN32
    if (absolute) {
        if (0 == path->len || ':' != path->str[1]) {
            g_string_prepend_c(path, PATH_SEP);
        }
    }

    ret = g_string_free (path, FALSE);
    canonicalize_pathname (ret);

    return ret;
}


/**
 *  Build filename from arguments.
 *  Like to g_build_filename(), but respect VFS_PATH_URL_DELIMITER
 */
char *
mc_build_filename(const char *first_element, ...)
{
    va_list args;
    char *ret;

    if (first_element == NULL)
        return NULL;
    va_start (args, first_element);
    ret = mc_build_filenamev (first_element, args);
    va_end (args);
    return ret;
}

/*end*/

