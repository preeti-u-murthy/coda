/* BLURB gpl

                           Coda File System
                              Release 6

          Copyright (c) 1987-2003 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights

#*/

/*
                         IBM COPYRIGHT NOTICE

                          Copyright (C) 1986
             International Business Machines Corporation
                         All Rights Reserved

This  file  contains  some  code identical to or derived from the 1986
version of the Andrew File System ("AFS"), which is owned by  the  IBM
Corporation.   This  code is provided "AS IS" and IBM does not warrant
that it is free of infringement of  any  intellectual  rights  of  any
third  party.    IBM  disclaims  liability of any kind for any damages
whatsoever resulting directly or indirectly from use of this  software
or  of  any  derivative work.  Carnegie Mellon University has obtained
permission to  modify,  distribute and sublicense this code,  which is
based on Version 2  of  AFS  and  does  not  contain  the features and
enhancements that are part of  Version 3 of  AFS.  Version 3 of AFS is
commercially   available   and  supported  by   Transarc  Corporation,
Pittsburgh, PA.

*/

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <pwd.h>
#include "auth2.h"
#include "avenus.h"
#include "codaconf.h"

#ifdef __CYGWIN32__
#include <windows.h>
#endif

#ifdef __cplusplus
}
#endif

#include <parse_realms.h>

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif

static int GetTokens(const char *realm)
{
    ClearToken clear;
    EncryptedSecretToken secret;
    time_t endtimestamp;
    int rc;

    fprintf(stdout, "    @%s\n", realm);

    /* Get the tokens.  */
    rc = U_GetLocalTokens(&clear, secret, realm);
    if (rc < 0) {
	if (rc == -ENOTCONN)
	    fprintf(stdout, "\tNot Authenticated\n");
	else
	    fprintf(stdout, "\tGetLocalTokens error (%d)\n", -rc);
	return -1;
    }

    fprintf(stdout, "\tCoda user id:\t %d\n", clear.ViceId);

    /* Check for expiration. */
    if (clear.EndTimestamp <= time(NULL)) {
	fprintf(stdout, "\tThis token has expired.\n");
	return -2;
    }
    endtimestamp = (time_t)clear.EndTimestamp;
    fprintf(stdout, "\tExpiration time: %s", ctime(&endtimestamp));
    return 0;
}


int main(int argc, char *argv[])
{
    const char *realm = NULL;
    char *username = NULL;
    int rc = 0;

#ifdef __CYGWIN__
    username = getlogin();
#else
    struct passwd *pw = getpwuid(geteuid());
    if (pw)
        username = pw->pw_name;
#endif

    if (argc == 2)
	SplitRealmFromName(argv[1], &realm);

    /* Header. */
    fprintf(stdout, "Tokens held by the Cache Manager for %s:\n", username);

    if (!realm) {
	const char *mountpoint = NULL;
	struct dirent *entry;
	DIR *dir;

	codaconf_init("venus.conf");

#ifdef __CYGWIN__
	char *winmount = NULL;
	char cygwinmount[15];
	CODACONF_STR(winmount, "mountpoint", "N:");
	snprintf(cygwinmount,15,"/cygdrive/%c", winmount[0]);
	mountpoint = cygwinmount;
#else
	CODACONF_STR(mountpoint, "mountpoint", "/coda");
#endif

	dir = opendir(mountpoint);
	if (!dir) {
	    perror("Failed to get list of realms");
	    exit(-1);
	}

	while ((entry = readdir(dir)) != NULL)
	    if (entry->d_name[0] != '.')
		(void)GetTokens(entry->d_name);

	closedir(dir);
    } else
	rc = GetTokens(realm);

    exit(rc ? EXIT_FAILURE : EXIT_SUCCESS);
}

