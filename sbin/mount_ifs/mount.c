/*-
 * Copyright (c) 1980, 1989, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static const char copyright[] =
"@(#) Copyright (c) 1980, 1989, 1993, 1994\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

#ifndef lint
#if 0
static char sccsid[] = "@(#)mount.c	8.25 (Berkeley) 5/8/95";
#endif
static const char rcsid[] =
  "$FreeBSD$";
#endif /* not lint */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fstab.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "extern.h"
#include "mntopts.h"
#include "pathnames.h"

/* `meta' options */
#define MOUNT_META_OPTION_FSTAB		"fstab"	
#define MOUNT_META_OPTION_CURRENT	"current"

int debug, fstab_style, verbose;

char   *catopt(char *, const char *);
struct statfs
       *getmntpt(const char *);
int	hasopt(const char *, const char *);
int	ismounted(struct fstab *, struct statfs *, int);
int	isremountable(const char *);
void	mangle(char *, int *, const char **);
char   *update_options(char *, char *, int);
int	mountfs(const char *, const char *, const char *,
			int, const char *, const char *);
void	remopt(char *, const char *);
void	prmount(struct statfs *);
void	putfsent(const struct statfs *);
void	usage(void);
char   *flags2opts(int);

/* Map from mount options to printable formats. */
static struct opt {
	int o_opt;
	const char *o_name;
} optnames[] = {
	{ MNT_ASYNC,		"asynchronous" },
	{ MNT_EXPORTED,		"NFS exported" },
	{ MNT_LOCAL,		"local" },
	{ MNT_NOATIME,		"noatime" },
	{ MNT_NODEV,		"nodev" },
	{ MNT_NOEXEC,		"noexec" },
	{ MNT_NOSUID,		"nosuid" },
	{ MNT_NOSYMFOLLOW,	"nosymfollow" },
	{ MNT_QUOTA,		"with quotas" },
	{ MNT_RDONLY,		"read-only" },
	{ MNT_SYNCHRONOUS,	"synchronous" },
	{ MNT_UNION,		"union" },
	{ MNT_NOCLUSTERR,	"noclusterr" },
	{ MNT_NOCLUSTERW,	"noclusterw" },
	{ MNT_SUIDDIR,		"suiddir" },
	{ MNT_SOFTDEP,		"soft-updates" },
	{ 0, NULL }
};

/*
 * List of VFS types that can be remounted without becoming mounted on top
 * of each other.
 * XXX Is this list correct?
 */
static const char *
remountable_fs_names[] = {
	"ufs", "ffs", "lfs", "ext2fs",
	0
};

int
main(argc, argv)
	int argc;
	char * const argv[];
{

        mount_ifs(argc, argv);
        /* NOTREACHED */
}

int
ismounted(fs, mntbuf, mntsize)
	struct fstab *fs;
	struct statfs *mntbuf;
	int mntsize;
{
	int i;

	if (fs->fs_file[0] == '/' && fs->fs_file[1] == '\0')
		/* the root filesystem can always be remounted */
		return (0);

	for (i = mntsize - 1; i >= 0; --i)
		if (strcmp(fs->fs_file, mntbuf[i].f_mntonname) == 0 &&
		    (!isremountable(fs->fs_vfstype) ||
		     strcmp(fs->fs_spec, mntbuf[i].f_mntfromname) == 0))
			return (1);
	return (0);
}

int
isremountable(vfsname)
	const char *vfsname;
{
	const char **cp;

	for (cp = remountable_fs_names; *cp; cp++)
		if (strcmp(*cp, vfsname) == 0)
			return (1);
	return (0);
}

int
hasopt(mntopts, option)
	const char *mntopts, *option;
{
	int negative, found;
	char *opt, *optbuf;

	if (option[0] == 'n' && option[1] == 'o') {
		negative = 1;
		option += 2;
	} else
		negative = 0;
	optbuf = strdup(mntopts);
	found = 0;
	for (opt = optbuf; (opt = strtok(opt, ",")) != NULL; opt = NULL) {
		if (opt[0] == 'n' && opt[1] == 'o') {
			if (!strcasecmp(opt + 2, option))
				found = negative;
		} else if (!strcasecmp(opt, option))
			found = !negative;
	}
	free(optbuf);
	return (found);
}

int
mountfs(vfstype, spec, name, flags, options, mntopts)
	const char *vfstype, *spec, *name, *options, *mntopts;
	int flags;
{
	/* List of directories containing mount_xxx subcommands. */
	static const char *edirs[] = {
		_PATH_SBIN,
		_PATH_USRSBIN,
		NULL
	};
	const char *argv[100], **edir;
	struct statfs sf;
	pid_t pid;
	int argc, i, status;
	char *optbuf, execname[MAXPATHLEN + 1], mntpath[MAXPATHLEN];

#if __GNUC__
	(void)&optbuf;
	(void)&name;
#endif

	/* resolve the mountpoint with realpath(3) */
	(void)checkpath(name, mntpath);
	name = mntpath;

	if (mntopts == NULL)
		mntopts = "";
	if (options == NULL) {
		if (*mntopts == '\0') {
			options = "rw";
		} else {
			options = mntopts;
			mntopts = "";
		}
	}
	optbuf = catopt(strdup(mntopts), options);

	if (strcmp(name, "/") == 0)
		flags |= MNT_UPDATE;
	if (flags & MNT_FORCE)
		optbuf = catopt(optbuf, "force");
	if (flags & MNT_RDONLY)
		optbuf = catopt(optbuf, "ro");
	/*
	 * XXX
	 * The mount_mfs (newfs) command uses -o to select the
	 * optimization mode.  We don't pass the default "-o rw"
	 * for that reason.
	 */
	if (flags & MNT_UPDATE)
		optbuf = catopt(optbuf, "update");

	argc = 0;
	argv[argc++] = vfstype;
	mangle(optbuf, &argc, argv);
	argv[argc++] = spec;
	argv[argc++] = name;
	argv[argc] = NULL;

	if (debug) {
		(void)printf("exec: mount_%s", vfstype);
		for (i = 1; i < argc; i++)
			(void)printf(" %s", argv[i]);
		(void)printf("\n");
		return (0);
	}

	switch (pid = fork()) {
	case -1:				/* Error. */
		warn("fork");
		free(optbuf);
		return (1);
	case 0:					/* Child. */
		if (strcmp(vfstype, "ifs") == 0)
			exit(mount_ifs(argc, (char * const *) argv));

		/* Go find an executable. */
		for (edir = edirs; *edir; edir++) {
			(void)snprintf(execname,
			    sizeof(execname), "%s/mount_%s", *edir, vfstype);
			execv(execname, (char * const *)argv);
		}
		if (errno == ENOENT) {
			int len = 0;
			char *cp;
			for (edir = edirs; *edir; edir++)
				len += strlen(*edir) + 2;	/* ", " */
			if ((cp = malloc(len)) == NULL)
				errx(1, "malloc failed");
			cp[0] = '\0';
			for (edir = edirs; *edir; edir++) {
				strcat(cp, *edir);
				if (edir[1] != NULL)
					strcat(cp, ", ");
			}
			warn("exec mount_%s not found in %s", vfstype, cp);
		}
		exit(1);
		/* NOTREACHED */
	default:				/* Parent. */
		free(optbuf);

		if (waitpid(pid, &status, 0) < 0) {
			warn("waitpid");
			return (1);
		}

		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) != 0)
				return (WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			warnx("%s: %s", name, sys_siglist[WTERMSIG(status)]);
			return (1);
		}

		if (verbose) {
			if (statfs(name, &sf) < 0) {
				warn("statfs %s", name);
				return (1);
			}
			if (fstab_style)
				putfsent(&sf);
			else
				prmount(&sf);
		}
		break;
	}

	return (0);
}

void
prmount(sfp)
	struct statfs *sfp;
{
	int flags;
	struct opt *o;
	struct passwd *pw;

	(void)printf("%s on %s (%s", sfp->f_mntfromname, sfp->f_mntonname,
	    sfp->f_fstypename);

	flags = sfp->f_flags & MNT_VISFLAGMASK;
	for (o = optnames; flags && o->o_opt; o++)
		if (flags & o->o_opt) {
			(void)printf(", %s", o->o_name);
			flags &= ~o->o_opt;
		}
	if (sfp->f_owner) {
		(void)printf(", mounted by ");
		if ((pw = getpwuid(sfp->f_owner)) != NULL)
			(void)printf("%s", pw->pw_name);
		else
			(void)printf("%d", sfp->f_owner);
	}
	if (verbose) {
		if (sfp->f_syncwrites != 0 || sfp->f_asyncwrites != 0)
			(void)printf(", writes: sync %ld async %ld",
			    sfp->f_syncwrites, sfp->f_asyncwrites);
		if (sfp->f_syncreads != 0 || sfp->f_asyncreads != 0)
			(void)printf(", reads: sync %ld async %ld",
			    sfp->f_syncreads, sfp->f_asyncreads);
	}
	(void)printf(")\n");
}

struct statfs *
getmntpt(name)
	const char *name;
{
	struct statfs *mntbuf;
	int i, mntsize;

	mntsize = getmntinfo(&mntbuf, MNT_NOWAIT);
	for (i = mntsize - 1; i >= 0; i--) {
		if (strcmp(mntbuf[i].f_mntfromname, name) == 0 ||
		    strcmp(mntbuf[i].f_mntonname, name) == 0)
			return (&mntbuf[i]);
	}
	return (NULL);
}

char *
catopt(s0, s1)
	char *s0;
	const char *s1;
{
	size_t i;
	char *cp;

	if (s1 == NULL || *s1 == '\0')
		return s0;

	if (s0 && *s0) {
		i = strlen(s0) + strlen(s1) + 1 + 1;
		if ((cp = malloc(i)) == NULL)
			errx(1, "malloc failed");
		(void)snprintf(cp, i, "%s,%s", s0, s1);
	} else
		cp = strdup(s1);

	if (s0)
		free(s0);
	return (cp);
}

void
mangle(options, argcp, argv)
	char *options;
	int *argcp;
	const char **argv;
{
	char *p, *s;
	int argc;

	argc = *argcp;
	for (s = options; (p = strsep(&s, ",")) != NULL;)
		if (*p != '\0') {
			if (*p == '-') {
				argv[argc++] = p;
				p = strchr(p, '=');
				if (p) {
					*p = '\0';
					argv[argc++] = p+1;
				}
			} else if (strcmp(p, "rw") != 0) {
				argv[argc++] = "-o";
				argv[argc++] = p;
			}
		}

	*argcp = argc;
}


char *
update_options(opts, fstab, curflags)
	char *opts;
	char *fstab;
	int curflags;
{
	char *o, *p;
	char *cur;
	char *expopt, *newopt, *tmpopt;

	if (opts == NULL)
		return strdup("");

	/* remove meta options from list */
	remopt(fstab, MOUNT_META_OPTION_FSTAB);
	remopt(fstab, MOUNT_META_OPTION_CURRENT);
	cur = flags2opts(curflags);

	/*
	 * Expand all meta-options passed to us first.
	 */
	expopt = NULL;
	for (p = opts; (o = strsep(&p, ",")) != NULL;) {
		if (strcmp(MOUNT_META_OPTION_FSTAB, o) == 0)
			expopt = catopt(expopt, fstab);
		else if (strcmp(MOUNT_META_OPTION_CURRENT, o) == 0)
			expopt = catopt(expopt, cur);
		else
			expopt = catopt(expopt, o);
	}
	free(cur);
	free(opts);

	/*
	 * Remove previous contradictory arguments. Given option "foo" we
	 * remove all the "nofoo" options. Given "nofoo" we remove "nonofoo"
	 * and "foo" - so we can deal with possible options like "notice".
	 */
	newopt = NULL;
	for (p = expopt; (o = strsep(&p, ",")) != NULL;) {
		if ((tmpopt = malloc( strlen(o) + 2 + 1 )) == NULL)
			errx(1, "malloc failed");
	
		strcpy(tmpopt, "no");
		strcat(tmpopt, o);
		remopt(newopt, tmpopt);
		free(tmpopt);

		if (strncmp("no", o, 2) == 0)
			remopt(newopt, o+2);

		newopt = catopt(newopt, o);
	}
	free(expopt);

	return newopt;
}

void
remopt(string, opt)
	char *string;
 	const char *opt;
{
	char *o, *p, *r;

	if (string == NULL || *string == '\0' || opt == NULL || *opt == '\0')
		return;

	r = string;

	for (p = string; (o = strsep(&p, ",")) != NULL;) {
		if (strcmp(opt, o) != 0) {
			if (*r == ',' && *o != '\0')
				r++;
			while ((*r++ = *o++) != '\0')
			    ;
			*--r = ',';
		}
	}
	*r = '\0';
}

void
usage()
{

	(void)fprintf(stderr, "%s\n",
"usage: mount [-dfpruvw] [-o options] special node");
	exit(1);
}

void
putfsent(ent)
	const struct statfs *ent;
{
	struct fstab *fst;
	char *opts;
  
	opts = flags2opts(ent->f_flags);
	printf("%s\t%s\t%s %s", ent->f_mntfromname, ent->f_mntonname,
	    ent->f_fstypename, opts);
	free(opts);

	if ((fst = getfsspec(ent->f_mntfromname)))
		printf("\t%u %u\n", fst->fs_freq, fst->fs_passno);
	else if ((fst = getfsfile(ent->f_mntonname)))
		printf("\t%u %u\n", fst->fs_freq, fst->fs_passno);
	else if (strcmp(ent->f_fstypename, "ufs") == 0) {
		if (strcmp(ent->f_mntonname, "/") == 0)
			printf("\t1 1\n");
		else
			printf("\t2 2\n");
	} else
		printf("\t0 0\n");
}


char *
flags2opts(flags)
	int flags;
{
	char *res;

	res = NULL;

	res = catopt(res, (flags & MNT_RDONLY) ? "ro" : "rw");

	if (flags & MNT_SYNCHRONOUS)	res = catopt(res, "sync");
	if (flags & MNT_NOEXEC)		res = catopt(res, "noexec");
	if (flags & MNT_NOSUID)		res = catopt(res, "nosuid");
	if (flags & MNT_NODEV)		res = catopt(res, "nodev");
	if (flags & MNT_UNION)		res = catopt(res, "union");
	if (flags & MNT_ASYNC)		res = catopt(res, "async");
	if (flags & MNT_NOATIME)	res = catopt(res, "noatime");
	if (flags & MNT_NOCLUSTERR)	res = catopt(res, "noclusterr");
	if (flags & MNT_NOCLUSTERW)	res = catopt(res, "noclusterw");
	if (flags & MNT_NOSYMFOLLOW)	res = catopt(res, "nosymfollow");
	if (flags & MNT_SUIDDIR)	res = catopt(res, "suiddir");

	return res;
}
