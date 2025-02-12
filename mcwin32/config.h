#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED
/*
 *  win32 Midnight Commander -- config.h
 *
 *  Written by: Adam Young 2012 - 2018
 *
 *  This file is part of the Midnight Commander.
 *
 *  The Midnight Commander is free software: you can redistribute it
 *  and/or modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation, either version 3 of the License,
 *  or (at your option) any later version.
 *
 *  The Midnight Commander is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  ==end==
 */

#include "w32config.h"                          /* common configuration */

#define WIN32_UNISTD_MAP                        /* enable unistd API mapping */
//  #define WIN32_SOCKET_MAP                    /* enable socket API mapping; not compatible with libssh */
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#if defined(_MSC_VER)
#pragma warning (disable : 4127)                /* conditional expression is constant */
#pragma warning (disable : 4201)                /* nonstandard extension used : nameless struct/union */
#pragma warning (disable : 4204)                /* nonstandard extension used : non-constant aggregate initializer */
#pragma warning (disable : 4702)                /* unreachable code */
#pragma warning (disable : 4706)                /* assignment within conditional expression */
#pragma warning (disable : 4996)                /* 'xxx' was declared deprecated */

#elif defined(__WATCOMC__)
#pragma disable_message(136)                    /* Comparison equivalent to 'unsigned == 0' */
#pragma disable_message(201)                    /* Unreachable code */
#pragma disable_message(202)                    /* Unreferenced */
#pragma disable_message(124)                    /* Comparison result always 0 */
#endif

/*
 *  build information
 */
#include "buildinfo.h"

#define MC_CONFIGURE_ARGS   "win32-native"
#define MC_APPLICATION_DIR  "Midnight Commander"

/*
 *  application runtime configuration
 */
#define MC_USERCONF_DIR     MC_APPLICATION_DIR  /* see: fileloc.h, default "mc" */
#undef  MC_HOMEDIR_XDG                          /* enforce Freedesktop recommended dirs, not required */

const char *                mc_TMPDIR(void);
const char *                mc_SYSCONFDIR(void);
const char *                mc_DATADIR(void);
const char *                mc_LOCALEDIR(void);
const char *                mc_MAGICPATH(void);
const char *                mc_LIBEXECDIR(void);
char *                      mc_USERCONFIGDIR(const char *subdir);
const char *                mc_EXTHELPERSDIR(void);

#define SYSCONFDIR          mc_SYSCONFDIR()     /* /etc/mc */
#define WIN32_DATADIR       mc_DATADIR()        /* /usr/share/mc */
#define LOCALEDIR           mc_LOCALEDIR()      /* /usr/share/locale */
#define LIBEXECDIR          mc_LIBEXECDIR()     /* /lib/mc */
#define EXTHELPERSDIR       mc_EXTHELPERSDIR()  /* ???, 4.8.7 */

extern FILE *               win32_popen(const char *cmd, const char *mode);
extern int                  win32_pclose(FILE *file);

#ifndef popen
#define popen(__cmd,__mode) win32_popen(__cmd, __mode)
#define pclose(__file)      win32_pclose(__file)
#endif

/*
 *  available components
 */
#define HAVE_SLANG 1
#undef  HAVE_SLANG_SLANG_H
#define HAVE_SLANG_H 1
#define HAVE_SYS_PARAM_H 1
#undef  HAVE_SYS_SELECT_H
#define HAVE_SYS_VFS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_TIME_H 1
#define HAVE_UTIME_H 1
#define HAVE_STDARG_H 1
#define HAVE_ASSERT_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1

#define HAVE_GETFSSTAT 1
#define HAVE_GETMNTINFO 1
#if defined(HAVE_GETMNTINFO)
#define HAVE_INFOMOUNT_LIST 1                   /* see: mountlist.c */
#define MOUNTED_GETMNTINFO 1
#endif

#define HAVE_LIBMAGIC
#undef  HAVE_ASPELL
#undef  HAVE_SUBSHELL_SUPPORT
#define HAVE_CHARSET 1
#define HAVE_SLANG 1
#undef  HAVE_TEXTMODE_X11_SUPPORT
#undef  HAVE_LIBGPM

#undef  HAVE_REALPATH                           /* FIXME */
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_GETOPT 1
#if defined(__WATCOMC__)
#define HAVE_STRLCPY 1
#define HAVE_STRLCAT 1
#define HAVE_LOCALE_H  1
#endif

/*
 *  configuration options
 */
#undef  SEARCH_TYPE_PCRE
#define SEARCH_TYPE_GLIB 1

#define LISTMODE_EDITOR 1
#define USE_INTERNAL_EDIT 1
#define USE_DIFF_VIEW 1
#define USE_LIBMAGIC 1
#undef  USE_MAINTAINER_MODE                     /* see: ../lib/logging.c/.h */
#define USE_SLANG 1
#undef  USE_NCURSES
#undef  USE_NCURSESW

#define ENABLE_NLS
#undef  ENABLE_BACKGROUND
#undef  ENABLE_SUBSHELL

#define ENABLE_VFS 1
#define ENABLE_VFS_CPIO 1
#define ENABLE_VFS_TAR 1
#define ENABLE_VFS_SFS 1
#define ENABLE_VFS_EXTFS 1
#define ENABLE_VFS_FTP 1
#undef  ENABLE_VFS_FISH
#define ENABLE_VFS_SFTP 1                       /* libssh2 */
#undef  ENABLE_VFS_SMB
#undef  ENABLE_VFS_UNDELFS

#define SIG_ATOMIC_VOLATILE_T int               /* FIXME */
#define PROMOTED_MODE_T int                     /* FIXME */

#endif  /*CONFIG_H_INCLUDED*/

