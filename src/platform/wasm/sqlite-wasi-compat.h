/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2025, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */

/*
 * Force-included ahead of the vendored sqlite3.c when compiling for wasi.
 *
 * sqlite's unix VFS is written against a fuller POSIX than wasi-libc offers:
 * it wants advisory record locks through fcntl() and the ownership calls
 * fchown() and geteuid(), none of which wasi has. wasi-libc leaves the
 * F_*LK commands undeclared and the functions out of unistd.h, so sqlite
 * fails to compile rather than to run.
 *
 * Declaring them here lets it compile with the same behaviour it has on a
 * filesystem that refuses locks: fcntl() rejects the lock commands, sqlite
 * reports a locking error if it ever tries one, and the ownership calls
 * report the same failure. None of that is reached unless the patch
 * database is actually opened, which the WASM build does not do on its own.
 */

#ifndef SURGE_SQLITE_WASI_COMPAT_H
#define SURGE_SQLITE_WASI_COMPAT_H

#ifdef __wasi__

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef F_RDLCK
#define F_RDLCK 0
#endif
#ifndef F_WRLCK
#define F_WRLCK 1
#endif
#ifndef F_UNLCK
#define F_UNLCK 2
#endif
#ifndef F_GETLK
#define F_GETLK 5
#endif
#ifndef F_SETLK
#define F_SETLK 6
#endif
#ifndef F_SETLKW
#define F_SETLKW 7
#endif

static inline int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    errno = ENOSYS;
    return -1;
}

static inline uid_t geteuid(void) { return 0; }

#endif // __wasi__

#endif // SURGE_SQLITE_WASI_COMPAT_H
