/*-
 * Copyright (c) 2009 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <prop/proplib.h>

#include <xbps_api.h>
#include "defs.h"

enum {
	TRANS_ONE,
	TRANS_ALL
};

struct transaction {
	prop_dictionary_t dict;
	prop_object_iterator_t iter;
	const char *originpkgname;
	const char *curpkgname;
	int type;
	bool force;
	bool update;
};

static void	show_missing_deps(prop_dictionary_t, const char *);
static int	show_missing_dep_cb(prop_object_t, void *, bool *);
static int	exec_transaction(struct transaction *);
static void	cleanup(int);

static void
show_missing_deps(prop_dictionary_t d, const char *pkgname)
{
	printf("Unable to locate some required packages for %s:\n",
	    pkgname);
	(void)xbps_callback_array_iter_in_dict(d, "missing_deps",
	    show_missing_dep_cb, NULL);
}

static int
show_missing_dep_cb(prop_object_t obj, void *arg, bool *loop_done)
{
	const char *pkgname, *version;

        (void)arg;
        (void)loop_done;

	prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
	prop_dictionary_get_cstring_nocopy(obj, "version", &version);
	if (pkgname && version) {
		printf("  * Missing binary package for: %s >= %s\n",
		    pkgname, version);
		return 0;
	}

	return EINVAL;
}

static int
check_pkg_hashes(prop_object_iterator_t iter)
{
	prop_object_t obj;
	const char *pkgname, *repoloc, *filename;
	int rv = 0;
	pkg_state_t state = 0;

	printf("Checking binary package file(s) integrity...\n");
	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
		state = 0;
		if (xbps_get_pkg_state_dictionary(obj, &state) != 0)
			return EINVAL;

		if (state == XBPS_PKG_STATE_UNPACKED)
			continue;

		prop_dictionary_get_cstring_nocopy(obj, "repository", &repoloc);
		prop_dictionary_get_cstring_nocopy(obj, "filename", &filename);
		rv = xbps_check_pkg_file_hash(obj, repoloc);
		if (rv != 0 && rv != ERANGE) {
			printf("Unexpected error while checking hash for "
			    "%s (%s)\n", filename, strerror(rv));
			return -1;
		} else if (rv != 0 && rv == ERANGE) {
			printf("Hash mismatch for %s, exiting.\n",
			    filename);
			return -1;
		}
	}
	prop_object_iterator_reset(iter);

	return 0;
}

static int
show_transaction_sizes(prop_object_iterator_t iter, const char *descr)
{
	prop_object_t obj;
	uint64_t tsize = 0, dlsize = 0, instsize = 0;
	size_t cols = 0;
	const char *pkgname, *version;
	char size[64];
	bool first = false;

	/*
	 * Iterate over the list of packages that are going to be
	 * installed and check the file hash.
	 */
	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		prop_dictionary_get_uint64(obj, "filename-size", &tsize);
		dlsize += tsize;
		tsize = 0;
		prop_dictionary_get_uint64(obj, "installed_size", &tsize);
		instsize += tsize;
		tsize = 0;
	}
	prop_object_iterator_reset(iter);

	/*
	 * Show the list of packages that will be installed.
	 */
	printf("\nThe following new packages will be %s:\n\n", descr);

	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(obj, "version", &version);
		cols += strlen(pkgname) + strlen(version) + 4;
		if (cols <= 80) {
			if (first == false) {
				printf("  ");
				first = true;
			}
		} else {
			printf("\n  ");
			cols = strlen(pkgname) + strlen(version) + 4;
		}
		printf("%s-%s ", pkgname, version);
	}
	prop_object_iterator_reset(iter);
	printf("\n\n");

	/*
	 * Show total download/installed size for all required packages.
	 */
	if (xbps_humanize_number(size, 5, (int64_t)dlsize,
	    "", HN_AUTOSCALE, HN_NOSPACE) == -1) {
		printf("error: humanize_number returns %s\n",
		    strerror(errno));
		return -1;
	}
	printf("Total download size: %s\n", size);
	if (xbps_humanize_number(size, 5, (int64_t)instsize,
	    "", HN_AUTOSCALE, HN_NOSPACE) == -1) {
		printf("error: humanize_number2 returns %s\n",
		    strerror(errno));
		return -1;
	}
	printf("Total installed size: %s\n\n", size);

	return 0;
}

void
xbps_install_pkg(const char *pkg, bool force, bool update)
{
	struct transaction *trans;
	prop_dictionary_t pkgd;
	prop_array_t array;
	int rv = 0;

	/*
	 * Find all required pkgs and sort the package transaction.
	 */
	pkgd = xbps_find_pkg_installed_from_plist(pkg);
	if (update) {
		if (pkgd) {
			if ((rv = xbps_find_new_pkg(pkg, pkgd)) == 0) {
				printf("Package '%s' is up to date.\n", pkg);
				prop_object_release(pkgd);
				cleanup(rv);
			}
			prop_object_release(pkgd);
		} else {
			printf("Package '%s' not installed.\n", pkg);
			cleanup(rv);
		}
	} else {
		if (pkgd) {
			printf("Package '%s' is already installed.\n", pkg);
			prop_object_release(pkgd);
			cleanup(rv);
		}
		rv = xbps_prepare_pkg(pkg);
		if (rv != 0 && rv == EAGAIN) {
			printf("unable to locate %s in repository pool.", pkg);
			cleanup(rv);
		} else if (rv != 0 && rv != ENOENT) {
			printf("unexpected error: %s", strerror(rv));
			cleanup(rv);
		}
	}

	trans = calloc(1, sizeof(struct transaction));
	if (trans == NULL)
		goto out;

	trans->dict = xbps_get_pkg_props();
	if (trans->dict == NULL) {
		printf("error: unexistent props dictionary!\n");
		goto out1;
	}

	/*
	 * Bail out if there are unresolved deps.
	 */
	array = prop_dictionary_get(trans->dict, "missing_deps");
	if (prop_array_count(array) > 0) {
		show_missing_deps(trans->dict, pkg);
		goto out2;
	}

	prop_dictionary_get_cstring_nocopy(trans->dict,
	     "origin", &trans->originpkgname);

	/*
	 * It's time to run the transaction!
	 */
	trans->iter = xbps_get_array_iter_from_dict(trans->dict, "packages");
	if (trans->iter == NULL) {
		printf("error: allocating array mem! (%s)",
		    strerror(errno));
		goto out2;
	}

	trans->force = force;
	trans->update = update;
	trans->type = TRANS_ONE;
	rv = exec_transaction(trans);

	prop_object_iterator_release(trans->iter);
out2:
	prop_object_release(trans->dict);
out1:
	free(trans);
out:
	cleanup(rv);
}

static int
exec_transaction(struct transaction *trans)
{
	prop_dictionary_t instpkgd;
	prop_object_t obj;
	const char *pkgname, *version, *instver, *filename;
	int rv = 0;
	bool essential, isdep;
	pkg_state_t state = 0;

	assert(trans != NULL);
	assert(trans->dict != NULL);
	assert(trans->iter != NULL);

	essential = isdep = false;
	/*
	 * Show download/installed size for the transaction.
	 */
	rv = show_transaction_sizes(trans->iter,
	    trans->type == TRANS_ALL ? "updated" : "installed");
	if (rv != 0)
		return rv;

	/*
	 * Ask interactively (if -f not set).
	 */
	if (trans->force == false) {
		if (xbps_noyes("Do you want to continue?") == false) {
			printf("Aborting!\n");
			return 0;
		}
	}

	/*
	 * Check the SHA256 hash for all required packages.
	 */
	if ((rv = check_pkg_hashes(trans->iter)) != 0)
		return rv;

	/*
	 * Iterate over the transaction dictionary.
	 */
	while ((obj = prop_object_iterator_next(trans->iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(obj, "version", &version);
		prop_dictionary_get_bool(obj, "essential", &essential);
		prop_dictionary_get_cstring_nocopy(obj, "filename", &filename);

		if ((trans->type == TRANS_ONE) &&
		    strcmp(trans->originpkgname, pkgname))
			isdep = true;

		/*
		 * If dependency is already unpacked skip this phase.
		 */
		state = 0;
		if (xbps_get_pkg_state_dictionary(obj, &state) != 0)
			return EINVAL;

		if (state == XBPS_PKG_STATE_UNPACKED)
			continue;

		if ((trans->type == TRANS_ALL) ||
		    (trans->update &&
		     strcmp(trans->curpkgname, pkgname) == 0)) {
			instpkgd = xbps_find_pkg_installed_from_plist(pkgname);
			if (instpkgd == NULL) {
				printf("error: unable to find %s installed "
				    "dict!\n", pkgname);
				return EINVAL;
			}

			prop_dictionary_get_cstring_nocopy(instpkgd,
			    "version", &instver);
			prop_object_release(instpkgd);

			/*
			 * If this package is not 'essential', just remove
			 * the old package and install the new one. Otherwise
			 * we just overwrite the files.
			 */
			if (essential == false) {
				rv = xbps_remove_pkg(pkgname, version, true);
				if (rv != 0) {
					printf("error: removing %s-%s (%s)\n",
					    pkgname, instver, strerror(rv));
					return rv;
				}
			}
		}
		/*
		 * Unpack binary package.
		 */
		printf("Unpacking %s-%s (from .../%s) ...\n", pkgname, version,
		    filename);
		if ((rv = xbps_unpack_binary_pkg(obj, essential)) != 0) {
			printf("error: unpacking %s-%s (%s)\n", pkgname,
			    version, strerror(rv));
			return rv;
		}
		/*
		 * Register binary package.
		 */
		if ((rv = xbps_register_pkg(obj, isdep)) != 0) {
			printf("error: registering %s-%s! (%s)\n",
			    pkgname, version, strerror(rv));
			return rv;
		}
		isdep = false;
		/*
		 * Set package state to unpacked in the transaction
		 * dictionary.
		 */
		if ((rv = xbps_set_pkg_state_dictionary(obj,
		    XBPS_PKG_STATE_UNPACKED)) != 0)
			return rv;
	}
	prop_object_iterator_reset(trans->iter);
	/*
	 * Configure all unpacked packages.
	 */
	while ((obj = prop_object_iterator_next(trans->iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(obj, "version", &version);
		printf("Configuring package %s-%s ...\n", pkgname, version);

		if ((rv = xbps_configure_pkg(pkgname, version)) != 0) {
			printf("Error configuring package %s-%s\n",
			    pkgname, version);
			return rv;
		}
	}

	return 0;
}

void
xbps_autoupdate_pkgs(bool force)
{
	struct transaction *trans;
	prop_dictionary_t dict;
	prop_object_t obj;
	const char *pkgname;
	int rv = 0;

	/*
	 * Prepare dictionary with all registered packages.
	 */
	dict = xbps_prepare_regpkgdb_dict();
	if (dict == NULL) {
		printf("No packages currently installed (%s).\n",
		    strerror(errno));
		cleanup(rv);
	}
	/*
	 * Prepare dictionary with all registered repositories.
	 */
	if ((rv = xbps_prepare_repolist_data()) != 0)
		goto out;

	/*
	 * Prepare transaction data.
	 */
	trans = calloc(1, sizeof(struct transaction));
	if (trans == NULL)
		goto out;

	trans->iter = xbps_get_array_iter_from_dict(dict, "packages");
	if (trans->iter == NULL) {
		rv = EINVAL;
		goto out1;
	}

	/*
	 * Find out if there is a newer version for all currently
	 * installed packages.
	 */
	while ((obj = prop_object_iterator_next(trans->iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
		rv = xbps_find_new_pkg(pkgname, obj);
		if (rv != 0) {
			prop_object_iterator_release(trans->iter);
			goto out1;
		}
	}
	prop_object_iterator_release(trans->iter);

	/*
	 * Get package transaction dictionary.
	 */
	trans->dict = xbps_get_pkg_props();
	if (trans->dict == NULL) {
		if (errno == 0) {
			printf("All packages are up-to-date.\n");
			goto out;
		}
		printf("Error while checking for new pkgs: %s\n",
		    strerror(errno));
		goto out1;
	}
	/*
	 * Sort the package transaction dictionary.
	 */
	if ((rv = xbps_sort_pkg_deps(trans->dict)) != 0) {
		printf("Error while sorting packages: %s\n",
		    strerror(rv));
		goto out2;
	}

	/*
	 * It's time to run the transaction!
	 */
	trans->iter = xbps_get_array_iter_from_dict(trans->dict, "packages");
	if (trans->iter == NULL) {
		printf("error: allocating array mem! (%s)\n", strerror(errno));
		goto out2;
	}

	trans->force = force;
	trans->update = true;
	trans->type = TRANS_ALL;
	rv = exec_transaction(trans);

	prop_object_iterator_release(trans->iter);
out2:
	prop_object_release(trans->dict);
out1:
	free(trans);
out:
	cleanup(rv);
}

static void
cleanup(int rv)
{
	xbps_release_repolist_data();
	xbps_release_regpkgdb_dict();
	exit(rv == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
