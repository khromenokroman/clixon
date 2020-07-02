/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>
#include <syslog.h>

#include <sys/stat.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

#include "clixon_err.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_log.h"
#include "clixon_file.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_nsctx.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"

/* List of plugins XXX 
 * 1. Place in clixon handle not global variables
 * 2. Use qelem circular lists
 */
static clixon_plugin *_clixon_plugins = NULL;  /* List of plugins (of client) */
static int            _clixon_nplugins = 0;  /* Number of plugins */

/*! Iterator over clixon plugins
 *
 * @note Never manipulate the plugin during operation or using the
 * same object recursively
 *
 * @param[in]  h       Clicon handle
 * @param[in] plugin   previous plugin, or NULL on init
 * @code
 *   clicon_plugin *cp = NULL;
 *   while ((cp = clixon_plugin_each(h, cp)) != NULL) {
 *     ...
 *   }
 * @endcode
 * @note Not optimized, alwasy iterates from the start of the list
 */
clixon_plugin *
clixon_plugin_each(clicon_handle  h,
		   clixon_plugin *cpprev)
{
    int            i;
    clixon_plugin *cp;
    clixon_plugin *cpnext = NULL; 

    if (cpprev == NULL)
	cpnext = _clixon_plugins;
    else{
	for (i = 0; i < _clixon_nplugins; i++) {
	    cp = &_clixon_plugins[i];
	    if (cp == cpprev)
		break;
	    cp = NULL;
	}
	if (cp && i < _clixon_nplugins-1)
	    cpnext = &_clixon_plugins[i+1];
    }
    return cpnext;
}

/*! Reverse iterator over clixon plugins, iterater from nr to 0
 *
 * @note Never manipulate the plugin during operation or using the
 * same object recursively
 *
 * @param[in]  h       Clicon handle
 * @param[in] plugin   previous plugin, or NULL on init
 * @code
 *   clicon_plugin *cp = NULL;
 *   while ((cp = clixon_plugin_each_revert(h, cp, nr)) != NULL) {
 *     ...
 *   }
 * @endcode
 * @note Not optimized, alwasy iterates from the start of the list
 */
clixon_plugin *
clixon_plugin_each_revert(clicon_handle  h,
			  clixon_plugin *cpprev,
			  int            nr)
{
    int            i;
    clixon_plugin *cp = NULL;
    clixon_plugin *cpnext = NULL; 

    if (cpprev == NULL){
	if (nr>0)
	    cpnext = &_clixon_plugins[nr-1];
    }
    else{
	for (i = nr-1; i >= 0; i--) {
	    cp = &_clixon_plugins[i];
	    if (cp == cpprev)
		break;
	    cp = NULL;
	}
	if (cp && i > 0)
	    cpnext = &_clixon_plugins[i-1];
    }
    return cpnext;
}

/*! Find plugin by name
 * @param[in]  h    Clicon handle
 * @param[in]  name Plugin name
 * @retval     p    Plugin if found
 * @retval     NULL Not found
 */
clixon_plugin *
clixon_plugin_find(clicon_handle h,
		   const char   *name)
{
    int            i;
    clixon_plugin *cp = NULL;

    for (i = 0; i < _clixon_nplugins; i++) {
	cp = &_clixon_plugins[i];
	if (strcmp(cp->cp_name, name) == 0)
	    return cp;
    }
    return NULL;
}

/*! Load a dynamic plugin object and call its init-function
 * @param[in]  h        Clicon handle
 * @param[in]  file     Which plugin to load
 * @param[in]  function Which function symbol to load and call
 * @param[in]  dlflags  See man(3) dlopen
 * @param[out] cpp      Clixon plugin structure (if retval is 1)
 * @retval     1        OK
 * @retval     0        Failed load, log, skip and continue with other plugins
 * @retval    -1        Error
 * @see clixon_plugins_load  Load all plugins
 */
static int
plugin_load_one(clicon_handle   h, 
		char           *file, /* note modified */
		const char     *function,
		int             dlflags,
		clixon_plugin **cpp)
{
    int                retval = -1;
    char              *error;
    void              *handle = NULL;
    plginit2_t        *initfn;
    clixon_plugin_api *api = NULL;
    clixon_plugin     *cp = NULL;
    char              *name;
    char              *p;

    clicon_debug(1, "%s file:%s function:%s", __FUNCTION__, file, function);
    dlerror();    /* Clear any existing error */
    if ((handle = dlopen(file, dlflags)) == NULL) {
        error = (char*)dlerror();
	clicon_err(OE_PLUGIN, errno, "dlopen: %s", error ? error : "Unknown error");
	goto done;
    }
    /* call plugin_init() if defined, eg CLIXON_PLUGIN_INIT or CLIXON_BACKEND_INIT */
    if ((initfn = dlsym(handle, function)) == NULL){
	clicon_err(OE_PLUGIN, errno, "Failed to find %s when loading clixon plugin %s", CLIXON_PLUGIN_INIT, file);
	goto done;
    }
    if ((error = (char*)dlerror()) != NULL) {
	clicon_err(OE_UNIX, 0, "dlsym: %s: %s", file, error);
	goto done;
    }
    clicon_err_reset();
    if ((api = initfn(h)) == NULL) {
	if (!clicon_errno){ 	/* if clicon_err() is not called then log and continue */
	    clicon_log(LOG_DEBUG, "Warning: failed to initiate %s", strrchr(file,'/')?strchr(file, '/'):file);
	    retval = 0;
	    goto done;
	}
	else{
	    clicon_err(OE_PLUGIN, errno, "Failed to initiate %s", strrchr(file,'/')?strchr(file, '/'):file);
	    goto done;
	}
    }
    /* Note: sizeof clixon_plugin_api which is largest of clixon_plugin_api:s */
    if ((cp = (clixon_plugin *)malloc(sizeof(struct clixon_plugin))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(cp, 0, sizeof(struct clixon_plugin));
    cp->cp_handle = handle;
    /* Extract string after last '/' in filename, if any */
    name = strrchr(file, '/') ? strrchr(file, '/')+1 : file;
    /* strip extension, eg .so from name */
    if ((p=strrchr(name, '.')) != NULL)
	*p = '\0';
    /* Copy name to struct */
    snprintf(cp->cp_name, sizeof(cp->cp_name), "%*s",
	     (int)strlen(name), name);
    cp->cp_api = *api;
    clicon_debug(1, "%s", __FUNCTION__);
    if (cp){
	*cpp = cp;
	cp = NULL;
    }
    retval = 1;
 done:
    if (retval != 1 && handle)
	dlclose(handle);
    if (cp)
	free(cp);
    return retval;
}

/*! Load a set of plugin objects from a directory and and call their init-function
 * @param[in]  h     Clicon handle
 * @param[in]  function Which function symbol to load and call (eg CLIXON_PLUGIN_INIT)
 * @param[in]  dir   Directory. .so files in this dir will be loaded.
 * @param[in]  regexp Regexp for matching files in plugin directory. Default *.so.
 * @retval     0     OK
 * @retval     -1    Error
 */
int
clixon_plugins_load(clicon_handle h,
    		    const char   *function,
		    const char   *dir,
    		    const char   *regexp)
{
    int            retval = -1;
    int            ndp;
    struct dirent *dp = NULL;
    int            i;
    char           filename[MAXPATHLEN];
    clixon_plugin *cp = NULL;
    int            ret;

    clicon_debug(1, "%s", __FUNCTION__); 
    /* Get plugin objects names from plugin directory */
    if((ndp = clicon_file_dirent(dir, &dp, regexp?regexp:"(.so)$", S_IFREG)) < 0)
	goto done;
    /* Load all plugins */
    for (i = 0; i < ndp; i++) {
	snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp[i].d_name);
	clicon_debug(1, "DEBUG: Loading plugin '%.*s' ...", 
		     (int)strlen(filename), filename);
	if ((ret = plugin_load_one(h, filename, function, RTLD_NOW, &cp)) < 0)
	    goto done;
	if (ret == 0)
	    continue;
	_clixon_nplugins++;
	if ((_clixon_plugins = realloc(_clixon_plugins, _clixon_nplugins*sizeof(clixon_plugin))) == NULL) {
	    clicon_err(OE_UNIX, errno, "realloc");
	    goto done;
	}
	_clixon_plugins[_clixon_nplugins-1] = *cp;
	free(cp);
    }
    retval = 0;
done:
    if (dp)
	free(dp);
    return retval;
}

/*! Create a pseudo plugin so that a main function can register callbacks
 * @param[in]  h     Clicon handle
 * @param[in]  name  Plugin name
 * @param[out] cpp   Clixon plugin structure (direct pointer)
 * @retval     0     OK, with cpp set
 * @retval    -1     Error
 */
int
clixon_pseudo_plugin(clicon_handle   h,
		     const char     *name,
		     clixon_plugin **cpp)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;

    clicon_debug(1, "%s", __FUNCTION__); 

    /* Create a pseudo plugins */
    /* Note: sizeof clixon_plugin_api which is largest of clixon_plugin_api:s */
    if ((cp = (clixon_plugin *)malloc(sizeof(struct clixon_plugin))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(cp, 0, sizeof(struct clixon_plugin));
    snprintf(cp->cp_name, sizeof(cp->cp_name), "%*s", (int)strlen(name), name);

    _clixon_nplugins++;
    if ((_clixon_plugins = realloc(_clixon_plugins, _clixon_nplugins*sizeof(clixon_plugin))) == NULL) {
	clicon_err(OE_UNIX, errno, "realloc");
	goto done;
    }
    _clixon_plugins[_clixon_nplugins-1] = *cp;
    *cpp = &_clixon_plugins[_clixon_nplugins-1];

    retval = 0;
done:
    if (cp)
	free(cp);
    return retval;
}

/*! Call single plugin start callback
 * @param[in]  cp      Plugin handle
 * @param[in]  h       Clixon handle
 * @retval     0       OK
 * @retval    -1       Error
 */
int
clixon_plugin_start_one(clixon_plugin *cp,
			clicon_handle  h)
{
    int          retval = -1;
    plgstart_t  *fn;          /* Plugin start */

    if ((fn = cp->cp_api.ca_start) != NULL){
	if (fn(h) < 0) {
	    if (clicon_errno < 0) 
		clicon_log(LOG_WARNING, "%s: Internal error: Start callback in plugin: %s returned -1 but did not make a clicon_err call",
			   __FUNCTION__, cp->cp_name);
	    goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Call plugin_start in all plugins
 * @param[in]  h       Clixon handle
 * Call plugin start functions (if defined)
 * @note  Start functions can use clicon_argv_get() to get -- command line options
 */
int
clixon_plugin_start_all(clicon_handle h)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;

    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if (clixon_plugin_start_one(cp, h) < 0)
	    goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Unload all plugins: call exit function and close shared handle
 * @param[in]  h       Clicon handle
 * @param[in]  cp      Plugin handle
 * @param[in]  h       Clixon handle
 * @retval     0       OK
 * @retval    -1       Error
 */
int
clixon_plugin_exit_one(clixon_plugin *cp,
		       clicon_handle  h)
{
    int          retval = -1;
    char        *error;
    plgexit_t   *fn;

    if ((fn = cp->cp_api.ca_exit) != NULL){
	if (fn(h) < 0) {
	    if (clicon_errno < 0) 
		clicon_log(LOG_WARNING, "%s: Internal error: Exit callback in plugin: %s returned -1 but did not make a clicon_err call",
			   __FUNCTION__, cp->cp_name);
	    goto done;
	}
	if (dlclose(cp->cp_handle) != 0) {
	    error = (char*)dlerror();
	    clicon_err(OE_PLUGIN, errno, "dlclose: %s", error ? error : "Unknown error");
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Unload all plugins: call exit function and close shared handle
 * @param[in]  h       Clixon handle
 * @retval     0       OK
 * @retval    -1       Error
 */
int
clixon_plugin_exit_all(clicon_handle h)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if (clixon_plugin_exit_one(cp, h) < 0)
	    goto done;
    }
    if (_clixon_plugins){
	free(_clixon_plugins);
	_clixon_plugins = NULL;
    }
    _clixon_nplugins = 0;
    retval = 0;
 done:
    return retval;
}

/*! Run the restconf user-defined credentials callback 
 * @param[in]  cp   Plugin handle
 * @param[in]  h    Clicon handle
 * @param[in]  arg  Argument, such as fastcgi handler for restconf
 * @retval    -1    Error
 * @retval     0    Not authenticated
 * @retval     1    Authenticated 
 * @note If authenticated either a callback was called and clicon_username_set() 
 *       Or no callback was found.
 */
int
clixon_plugin_auth_one(clixon_plugin *cp,
		       clicon_handle h, 
		       void         *arg)
{
    int        retval = 1;  /* Authenticated */
    plgauth_t *fn;          /* Plugin auth */

    if ((fn = cp->cp_api.ca_auth) != NULL){
	if ((retval = fn(h, arg)) < 0) {
	    if (clicon_errno < 0) 
		clicon_log(LOG_WARNING, "%s: Internal error: Auth callback in plugin: %s returned -1 but did not make a clicon_err call",
			   __FUNCTION__, cp->cp_name);
	    goto done;
	}
    }
 done:
    return retval;
}

/*! Run the restconf user-defined credentials callback for all plugins
 * Find first authentication callback and call that, then return.
 * The callback is to set the authenticated user
 * @param[in]  cp      Plugin handle
 * @param[in]  h    Clicon handle
 * @param[in]  arg  Argument, such as fastcgi handler for restconf
 * @retval    -1    Error
 * @retval     0    Not authenticated
 * @retval     1    Authenticated 
 * @note If authenticated either a callback was called and clicon_username_set() 
 *       Or no callback was found.
 */
int
clixon_plugin_auth_all(clicon_handle h, 
		       void         *arg)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;
    int            i = 0;
    int            ret;
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	i++;
	if ((ret = clixon_plugin_auth_one(cp, h, arg)) < 0)
	    goto done;
	if (ret == 1)
	    goto authenticated;
	break;
    }
    if (i==0)
	retval = 1;
    else
	retval = 0;
 done:
    return retval;
 authenticated:
    retval = 1;
    goto done;
}

/*! Callback for a yang extension (unknown) statement single plugin
 * extension can be made.
 * @param[in] cp   Plugin handle
 * @param[in] h    Clixon handle
 * @param[in] yext Yang node of extension 
 * @param[in] ys   Yang node of (unknown) statement belonging to extension
 * @retval    0    OK, 
 * @retval   -1    Error 
 */
int
clixon_plugin_extension_one(clixon_plugin *cp,
			    clicon_handle  h, 
			    yang_stmt     *yext,
			    yang_stmt     *ys)
{
    int             retval = 1;
    plgextension_t *fn;          /* Plugin extension fn */
    
    if ((fn = cp->cp_api.ca_extension) != NULL){
	if (fn(h, yext, ys) < 0) {
	    if (clicon_errno < 0) 
		clicon_log(LOG_WARNING, "%s: Internal error: Extension callback in plugin: %s returned -1 but did not make a clicon_err call",
			   __FUNCTION__, cp->cp_name);
	    goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Callback for a yang extension (unknown) statement in all plugins
 * Called at parsing of yang module containing a statement of an extension.
 * A plugin may identify the extension and perform actions
 * on the yang statement, such as transforming the yang.
 * A callback is made for every statement, which means that several calls per
 * extension can be made.
 * @param[in] h    Clixon handle
 * @param[in] yext Yang node of extension 
 * @param[in] ys   Yang node of (unknown) statement belonging to extension
 * @retval     0   OK, all callbacks executed OK
 * @retval    -1   Error in one callback
 */
int
clixon_plugin_extension_all(clicon_handle h,
			    yang_stmt    *yext,
			    yang_stmt    *ys)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if (clixon_plugin_extension_one(cp, h, yext, ys) < 0)
	    goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Call plugin general-purpose datastore upgrade in one plugin
 *
 * @param[in] cp   Plugin handle
 * @param[in] h    Clicon handle
 * @param[in] db   Name of datastore, eg "running", "startup" or "tmp"
 * @param[in] xt   XML tree. Upgrade this "in place"
 * @param[in] msd  Module-state diff, info on datastore module-state
 * @retval   -1    Error
 * @retval    0    OK
 * Upgrade datastore on load before or as an alternative to module-specific upgrading mechanism
 */
int
clixon_plugin_datastore_upgrade_one(clixon_plugin   *cp,
				    clicon_handle    h,
				    const char      *db,
				    cxobj           *xt,
				    modstate_diff_t *msd)

{
    int                  retval = -1;
    datastore_upgrade_t *fn;
    
    if ((fn = cp->cp_api.ca_datastore_upgrade) != NULL){
	if (fn(h, db, xt, msd) < 0) {
	    if (clicon_errno < 0) 
		clicon_log(LOG_WARNING, "%s: Internal error: Datastore upgrade callback in plugin: %s returned -1 but did not make a clicon_err call",
			   __FUNCTION__, cp->cp_name);
	    goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Call plugin general-purpose datastore upgrade in all plugins
 *
 * @param[in] h    Clicon handle
 * @param[in] db   Name of datastore, eg "running", "startup" or "tmp"
 * @param[in] xt   XML tree. Upgrade this "in place"
 * @param[in] msd  Module-state diff, info on datastore module-state
 * @retval   -1    Error
 * @retval    0    OK
 * Upgrade datastore on load before or as an alternative to module-specific upgrading mechanism
 */
int
clixon_plugin_datastore_upgrade_all(clicon_handle    h,
				    const char      *db,
				    cxobj           *xt,
				    modstate_diff_t *msd)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if (clixon_plugin_datastore_upgrade_one(cp, h, db, xt, msd) < 0)
	    goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*--------------------------------------------------------------------
 * RPC callbacks for both client/frontend and backend plugins.
 * RPC callbacks are explicitly registered in the plugin_init() function
 * with a tag and a function
 * When the the tag is encountered, the callback is called.
 * Primarily backend, but also netconf and restconf frontend plugins.
 * CLI frontend so far have direct callbacks, ie functions in the cligen
 * specification are directly dlsym:ed to the CLI plugin.
 * It would be possible to use this rpc registering API for CLI plugins as well.
 * 
 * When namespace and name match, the callback is made
 */
typedef struct {
    qelem_t       rc_qelem;	/* List header */
    clicon_rpc_cb rc_callback;  /* RPC Callback */
    void         *rc_arg;	/* Application specific argument to cb */
    char         *rc_namespace;/* Namespace to combine with name tag */
    char         *rc_name;	/* Xml/json tag/name */
} rpc_callback_t;

/* List of rpc callback entries XXX hang on handle */
static rpc_callback_t *rpc_cb_list = NULL;

/*! Register a RPC callback by appending a new RPC to the list
 *
 * @param[in]  h         clicon handle
 * @param[in]  cb        Callback called 
 * @param[in]  arg       Domain-specific argument to send to callback 
 * @param[in]  ns        namespace of rpc
 * @param[in]  name      RPC name
 * @retval     0         OK
 * @retval    -1         Error
 * @see rpc_callback_call  which makes the actual callback
 */
int
rpc_callback_register(clicon_handle  h,
		      clicon_rpc_cb  cb,
		      void          *arg,       
    		      const char    *ns,
		      const char    *name)
{
    rpc_callback_t *rc = NULL;

    if (name == NULL || ns == NULL){
	clicon_err(OE_DB, EINVAL, "name or namespace NULL");
	goto done;
    }
    if ((rc = malloc(sizeof(rpc_callback_t))) == NULL) {
	clicon_err(OE_DB, errno, "malloc: %s", strerror(errno));
	goto done;
    }
    memset(rc, 0, sizeof(*rc));
    rc->rc_callback = cb;
    rc->rc_arg  = arg;
    rc->rc_namespace  = strdup(ns);
    rc->rc_name  = strdup(name);
    ADDQ(rc, rpc_cb_list);
    return 0;
 done:
    if (rc){
	if (rc->rc_namespace)
	    free(rc->rc_namespace);
	if (rc->rc_name)
	    free(rc->rc_name);
	free(rc);
    }
    return -1;
}

/*! Delete all RPC callbacks
 */
int
rpc_callback_delete_all(clicon_handle h)
{
    rpc_callback_t *rc;

    while((rc = rpc_cb_list) != NULL) {
	DELQ(rc, rpc_cb_list, rpc_callback_t *);
	if (rc->rc_namespace)
	    free(rc->rc_namespace);
	if (rc->rc_name)
	    free(rc->rc_name);
	free(rc);
    }
    return 0;
}

/*! Search RPC callbacks and invoke if XML match with tag
 *
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at child of rpc: <rpc><xn></rpc>.
 * @param[out] cbret   Return XML (as string in CLIgen buffer), error or OK
 * @param[in]  arg     Domain-speific arg (eg client_entry)
 * @retval -1   Error
 * @retval  0   OK, not found handler.
 * @retval  n   OK, <n> handler called 
 * @see rpc_callback_register  which register a callback function
 * @note that several callbacks can be registered. They need to cooperate on
 * return values, ie if one writes cbret, the other needs to handle that by
 * leaving it, replacing it or amending it.
 */
int
rpc_callback_call(clicon_handle h,
		  cxobj        *xe, 
		  cbuf         *cbret,
		  void         *arg)
{
    int            retval = -1;
    rpc_callback_t *rc;
    char           *name;
    char           *prefix;
    char           *ns;
    int             nr = 0; /* How many callbacks */

    if (rpc_cb_list == NULL)
	return 0;
    name = xml_name(xe);
    prefix = xml_prefix(xe);
    xml2ns(xe, prefix, &ns);
    rc = rpc_cb_list;
    do {
	if (strcmp(rc->rc_name, name) == 0 &&
	    ns && rc->rc_namespace &&
	    strcmp(rc->rc_namespace, ns) == 0){
	    if (rc->rc_callback(h, xe, cbret, arg, rc->rc_arg) < 0){
		clicon_debug(1, "%s Error in: %s", __FUNCTION__, rc->rc_name);
		goto done;
	    }
	    nr++;
	}
	rc = NEXTQ(rpc_callback_t *, rc);
    } while (rc != rpc_cb_list);
    retval = nr; /* 0: none found, >0 nr of handlers called */
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

/*--------------------------------------------------------------------
 * Upgrade callbacks for backend upgrade of datastore
 * Register upgrade callbacks in plugin_init() with a module and a "from" and "to"
 * revision.
 */
typedef struct {
    qelem_t           uc_qelem;	    /* List header */
    clicon_upgrade_cb uc_callback;  /* RPC Callback */
    const char       *uc_fnstr;     /* Stringified fn name for debug */
    void             *uc_arg;	    /* Application specific argument to cb */
    char             *uc_namespace; /* Module namespace */
} upgrade_callback_t;

/* List of rpc callback entries XXX hang on handle */
static upgrade_callback_t *upgrade_cb_list = NULL;

/*! Register an upgrade callback by appending the new callback to the list
 *
 * @param[in]  h         clicon handle
 * @param[in]  cb        Callback called 
 * @param[in]  fnstr     Stringified function for debug
 * @param[in]  arg       Domain-specific argument to send to callback 
 * @param[in]  ns        Module namespace (if NULL all modules)
 * @retval     0         OK
 * @retval    -1         Error
 * @see upgrade_callback_call  which makes the actual callback
 */
int
upgrade_callback_reg_fn(clicon_handle     h,
			clicon_upgrade_cb cb,
			const char       *fnstr,
			const char       *ns,
			void             *arg)
{
    upgrade_callback_t *uc;

    if ((uc = malloc(sizeof(upgrade_callback_t))) == NULL) {
	clicon_err(OE_DB, errno, "malloc: %s", strerror(errno));
	goto done;
    }
    memset(uc, 0, sizeof(*uc));
    uc->uc_callback = cb;
    uc->uc_fnstr = fnstr;
    uc->uc_arg  = arg;
    if (ns)
	uc->uc_namespace  = strdup(ns);
    ADDQ(uc, upgrade_cb_list);
    return 0;
 done:
    if (uc){
	if (uc->uc_namespace)
	    free(uc->uc_namespace);
	free(uc);
    }
    return -1;
}

/*! Delete all Upgrade callbacks
 */
int
upgrade_callback_delete_all(clicon_handle h)
{
    upgrade_callback_t *uc;

    while((uc = upgrade_cb_list) != NULL) {
	DELQ(uc, upgrade_cb_list, upgrade_callback_t *);
	if (uc->uc_namespace)
	    free(uc->uc_namespace);
	free(uc);
    }
    return 0;
}

/*! Upgrade specific module identified by namespace, search matching callbacks
 *
 * @param[in]  h       clicon handle
 * @param[in]  xt      Top-level XML tree to be updated (includes other ns as well)
 * @param[in]  ns      Namespace of module
 * @param[in]  op      One of XML_FLAG_ADD, _DEL or _CHANGE
 * @param[in]  from    From revision on the form YYYYMMDD (if DEL or CHANGE)
 * @param[in]  to      To revision on the form YYYYMMDD (if ADD or CHANGE)
 * @param[out] cbret   Return XML (as string in CLIgen buffer), on invalid
 * @retval -1  Error
 * @retval  0  Invalid - cbret contains reason as netconf
 * @retval  1  OK
 * @see upgrade_callback_reg_fn  which registers the callbacks
 */
int
upgrade_callback_call(clicon_handle h,
		      cxobj        *xt,
		      char         *ns,
		      uint16_t      op,
		      uint32_t      from,
		      uint32_t      to,
		      cbuf         *cbret)
{
    int                 retval = -1;
    upgrade_callback_t *uc;
    int                 nr = 0; /* How many callbacks */
    int                 ret;

    if (upgrade_cb_list == NULL)
	return 1;
    uc = upgrade_cb_list;
    do {
	/* For matching an upgrade callback:
	 * - No module name registered (matches all modules) OR
	 * - Names match
	 * AND
	 * - No registered from revision (matches all revisions) OR
	 *   - Registered from revision >= from AND
         *   - Registered to revision <= to (which includes case both 0)
	 */
	if (uc->uc_namespace == NULL || strcmp(uc->uc_namespace, ns)==0){
	    if ((ret = uc->uc_callback(h, xt, ns, op, from, to, uc->uc_arg, cbret)) < 0){
		clicon_debug(1, "%s Error in: %s", __FUNCTION__, uc->uc_namespace);
		goto done;
	    }
	    if (ret == 0){
		if (cbuf_len(cbret)==0){	
		    clicon_err(OE_CFG, 0, "Validation fail %s(%s): cbret not set",
			       uc->uc_fnstr, ns);
		    goto done;
		}
		goto fail;
	    }
	    nr++;
	}
	uc = NEXTQ(upgrade_callback_t *, uc);
    } while (uc != upgrade_cb_list);
    retval = 1;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
 fail:
    retval =0;
    goto done;
}

