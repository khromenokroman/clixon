/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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

 * 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif 
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Exported functions in this file are in clixon_cli_api.h */
#include "clixon_cli_api.h"
#include "cli_common.h" /* internal functions */

/*! Completion callback intended for automatically generated data model
 *
 * Returns an expand-type list of commands as used by cligen 'expand' 
 * functionality.
 *
 * Assume callback given in a cligen spec: a <x:int expand_dbvar("arg")
 * @param[in]   h        clicon handle 
 * @param[in]   name     Name of this function (eg "expand_dbvar")
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   argv     Arguments given at the callback ("<db>" "<xmlkeyfmt>")
 * @param[out]  len      len of return commands & helptxt 
 * @param[out]  commands vector of function pointers to callback functions
 * @param[out]  helptxt  vector of pointers to helptexts
 * @see cli_expand_var_generate  This is where arg is generated
 * XXX: helptexts?
 */
int
expand_dbvar(void   *h, 
	     char   *name, 
	     cvec   *cvv, 
	     cvec   *argv, 
	     cvec  *commands,
	     cvec  *helptexts)
{
    int              retval = -1;
    char            *api_path;
    char            *dbstr;    
    cxobj           *xt = NULL;
    char            *xpath = NULL;
    cxobj          **xvec = NULL;
    cxobj           *xerr;
    size_t           xlen = 0;
    cxobj           *x;
    char            *bodystr;
    int              i;
    int              j;
    int              k;
    cg_var          *cv;
    yang_spec       *yspec;
    cxobj           *xtop = NULL; /* xpath root */
    cxobj           *xbot = NULL; /* xpath, NULL if datastore */
    yang_node       *y = NULL; /* yang spec of xpath */
    yang_stmt       *ytype;
    yang_stmt       *ypath;
    cxobj           *xcur;
    char            *xpathcur;

    if (argv == NULL || cvec_len(argv) != 2){
	clicon_err(OE_PLUGIN, 0, "%s: requires arguments: <db> <xmlkeyfmt>",
		   __FUNCTION__);
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: Error when accessing argument <db>");
	goto done;
    }
    dbstr  = cv_string_get(cv);
    if (strcmp(dbstr, "running") != 0 &&
	strcmp(dbstr, "candidate") != 0 &&
	strcmp(dbstr, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if ((cv = cvec_i(argv, 1)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: Error when accessing argument <api_path>");
	goto done;
    }
    api_path = cv_string_get(cv);
    /* api_path = /interface/%s/address/%s
       --> ^/interface/eth0/address/.*$
       --> /interface/[name=eth0]/address
    */
    if (api_path_fmt2xpath(api_path, cvv, &xpath) < 0)
	goto done;   
    /* XXX read whole configuration, why not send xpath? */
    if (clicon_rpc_get_config(h, dbstr, "/", &xt) < 0)
    	goto done;
    if ((xerr = xpath_first(xt, "/rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr);
	goto done;
    }
    xcur = xt; /* default top-of-tree */
    xpathcur = xpath;
    /* Create config top-of-tree */
    if ((xtop = xml_new("config", NULL)) == NULL)
	goto done;
    xbot = xtop;
    if (api_path && api_path2xml(api_path, yspec, xtop, &xbot, &y) < 0)
	goto done;
    /* Special case for leafref. Detect leafref via Yang-type, 
     * Get Yang path element, tentatively add the new syntax to the whole
     * tree and apply the path to that.
     * Last, the reference point for the xpath code below is changed to 
     * the point of the tentative new xml.
     * Here the whole syntax tree is loaded, and it would be better to offload
     * such operations to the datastore by a generic xpath function.
     */
    if ((ytype = yang_find((yang_node*)y, Y_TYPE, NULL)) != NULL)
	if (strcmp(ytype->ys_argument, "leafref")==0){
	    if ((ypath = yang_find((yang_node*)ytype, Y_PATH, NULL)) == NULL){
		clicon_err(OE_DB, 0, "Leafref %s requires path statement", ytype->ys_argument);
		goto done;
	    }
	    xpathcur = ypath->ys_argument;
	    if (xml_merge(xt, xtop, yspec) < 0)
		goto done;
	    if ((xcur = xpath_first(xt, xpath)) == NULL){
		clicon_err(OE_DB, 0, "xpath %s should return merged content", xpath);
		goto done;
	    }
	}
    /* One round to detect duplicates 
     */
    j = 0;
    if (xpath_vec(xcur, xpathcur, &xvec, &xlen) < 0) 
	goto done;
    for (i = 0; i < xlen; i++) {
	char *str;
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL){
	    clicon_err(OE_CFG, 0, "No xml body");
	    goto done;
	}
	/* detect duplicates */
	for (k=0; k<j; k++){
	    if (xml_type(xvec[k]) == CX_BODY)
		str = xml_value(xvec[k]);
	    else
		str = xml_body(xvec[k]);
	    if (strcmp(str, bodystr)==0)
		break;
	}
	if (k==j) /* not duplicate */
	    xvec[j++] = x;
    }
    xlen = j;
    for (i = 0; i < xlen; i++) {
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL){
	    clicon_err(OE_CFG, 0, "No xml body");
	    goto done;
	}
	/* XXX RFC3986 decode */
	cvec_add_string(commands, NULL, bodystr);
    }
    retval = 0;
  done:
    if (xvec)
	free(xvec);
    if (xtop)
	xml_free(xtop);
    if (xt)
	xml_free(xt);
    if (xpath) 
	free(xpath);
    return retval;
}
int
expandv_dbvar(void   *h, 
	      char   *name, 
	      cvec   *cvv, 
	      cvec   *argv, 
	      cvec  *commands,
	      cvec  *helptexts)
{
    return expand_dbvar(h, name, cvv, argv, commands, helptexts);
}
/*! List files in a directory
 */
int
expand_dir(char *dir, 
	   int *nr, 
	   char ***commands, 
	   mode_t flags, 
	   int detail)
{
    DIR	          *dirp;
    struct dirent *dp;
    struct stat    st;
    char          *str;
    char          *cmd;
    int            len;
    int            retval = -1;
    struct passwd *pw;
    char           filename[MAXPATHLEN];

    if ((dirp = opendir(dir)) == 0){
	fprintf(stderr, "expand_dir: opendir(%s) %s\n", 
		dir, strerror(errno));
	return -1;
    }
    *nr = 0;
    while ((dp = readdir(dirp)) != NULL) {
	if (
#if 0
	    strcmp(dp->d_name, ".") != 0 &&
	    strcmp(dp->d_name, "..") != 0
#else
	    dp->d_name[0] != '.'
#endif	    
	    ) {
	    snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp->d_name);
	    if (lstat(filename, &st) == 0){
		if ((st.st_mode & flags) == 0)
		    continue;

#if EXPAND_RECURSIVE
		if (S_ISDIR(st.st_mode)) {
		    int nrsav = *nr;
		    if(expand_dir(filename, nr, commands, detail) < 0)
			goto quit;
		    while(nrsav < *nr) {
			len = strlen(dp->d_name) +  strlen((*commands)[nrsav]) + 2;
			if((str = malloc(len)) == NULL) {
			    fprintf(stderr, "expand_dir: malloc: %s\n",
				    strerror(errno));
			    goto quit;
			}
			snprintf(str, len-1, "%s/%s",
				 dp->d_name, (*commands)[nrsav]);
			free((*commands)[nrsav]);
			(*commands)[nrsav] = str;
			
			nrsav++;
		    }
		    continue;
		}
#endif
		if ((cmd = strdup(dp->d_name)) == NULL) {
		    fprintf(stderr, "expand_dir: strdup: %s\n",
			    strerror(errno));
		    goto quit;
		}
		if (0 &&detail){
		    if ((pw = getpwuid(st.st_uid)) == NULL){
			fprintf(stderr, "expand_dir: getpwuid(%d): %s\n",
				st.st_uid, strerror(errno));
			goto quit;
		    }
		    len = strlen(cmd) + 
			strlen(pw->pw_name) +
#ifdef __FreeBSD__
			strlen(ctime(&st.st_mtimespec.tv_sec)) +
#else
			strlen(ctime(&st.st_mtim.tv_sec)) +
#endif

			strlen("{ by }") + 1 /* \0 */;
		    if ((str=realloc(cmd, strlen(cmd)+len)) == NULL) {
			fprintf(stderr, "expand_dir: malloc: %s\n",
				strerror(errno));
			goto quit;
		    }
		    snprintf(str + strlen(dp->d_name), 
			     len - strlen(dp->d_name),
			     "{%s by %s}",
#ifdef __FreeBSD__
			     ctime(&st.st_mtimespec.tv_sec),
#else
			     ctime(&st.st_mtim.tv_sec),
#endif

			     pw->pw_name
			);
		    cmd = str;
		}
		if (((*commands) =
		     realloc(*commands, ((*nr)+1)*sizeof(char**))) == NULL){
		    perror("expand_dir: realloc");
		    goto quit;
		}
		(*commands)[(*nr)] = cmd;
		(*nr)++;
		if (*nr >= 128) /* Limit number of options */
		    break;
	    }
	}
    }
    retval = 0;
  quit:
    closedir(dirp);
    return retval;
}

/*! CLI callback show yang spec. If arg given matches yang argument string */
int
show_yang(clicon_handle h, 
	  cvec         *cvv, 
	  cvec         *argv)
{
  yang_node *yn;
  char      *str = NULL;
  yang_spec *yspec;

  yspec = clicon_dbspec_yang(h);	
  if (cvec_len(argv) > 0){
      str = cv_string_get(cvec_i(argv, 0));
      yn = (yang_node*)yang_find((yang_node*)yspec, 0, str);
  }
  else
    yn = (yang_node*)yspec;
  yang_print(stdout, yn, 0);
  return 0;
}
int show_yangv(clicon_handle h, cvec *vars, cvec *argv)
{
    return show_yang(h, vars, argv);
}

/*! Generic show configuration CLIGEN callback
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <dbname>  "running"|"candidate"|"startup"
 *   <dbname>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <xpath>   xpath expression, that may contain one %, eg "/sender[name=%s]"
 *   <varname> optional name of variable in cvv. If set, xpath must have a '%s'
 * @code
 *   show config id <n:string>, cli_show_config("running","xml","iface[name=%s]","n");
 * @endcode
 */
int
cli_show_config(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    int              retval = -1;
    char            *db;
    char            *formatstr;
    char            *xpath;
    enum format_enum format;
    cbuf            *cbxpath = NULL;
    char            *attr = NULL;
    int              i;
    int              j;
    cg_var          *cvattr;
    char            *val = NULL;
    cxobj           *xt = NULL;
    cxobj           *xc;
    cxobj           *xerr;
    enum genmodel_type gt;
    
    if (cvec_len(argv) != 3 && cvec_len(argv) != 4){
	clicon_err(OE_PLUGIN, 0, "Got %d arguments. Expected: <dbname>,<format>,<xpath>[,<attr>]", cvec_len(argv));

	goto done;
    }
    /* First argv argument: Database */
    db = cv_string_get(cvec_i(argv, 0));
    /* Second argv argument: Format */
    formatstr = cv_string_get(cvec_i(argv, 1));
    if ((format = format_str2int(formatstr)) < 0){
	clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
	goto done;
    }
    /* Third argv argument: xpath */
    xpath = cv_string_get(cvec_i(argv, 2));

    /* Create XPATH variable string */
    if ((cbxpath = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");	
	goto done;
    }
    /* Fourth argument is stdarg to xpath format string */
    if (cvec_len(argv) == 4){
	attr = cv_string_get(cvec_i(argv, 3));
	j = 0;
	for (i=0; i<strlen(xpath); i++)
	    if (xpath[i] == '%')
		j++;
	if (j != 1){
	    clicon_err(OE_PLUGIN, 0, "xpath '%s' does not have a single '%%'");	
	    goto done;
	}
	if ((cvattr = cvec_find_var(cvv, attr)) == NULL){
	    clicon_err(OE_PLUGIN, 0, "attr '%s' not found in cligen var list", attr);	
	    goto done;
	}
	if ((val = cv2str_dup(cvattr)) == NULL){
	    clicon_err(OE_PLUGIN, errno, "cv2str_dup");	
	    goto done;
	}
	cprintf(cbxpath, xpath, val);	
    }
    else
	cprintf(cbxpath, "%s", xpath);	
    /* Get configuration from database */
    if (clicon_rpc_get_config(h, db, cbuf_get(cbxpath), &xt) < 0)
	goto done;
    if ((xerr = xpath_first(xt, "/rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr);
	goto done;
    }
    /* Print configuration according to format */
    switch (format){
    case FORMAT_XML:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    clicon_xml2file(stdout, xc, 0, 1);
	break;
    case FORMAT_JSON:
	xml2json(stdout, xt, 1);
	break;
    case FORMAT_TEXT:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    xml2txt(stdout, xc, 0); /* tree-formed text */
	break;
    case FORMAT_CLI:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL){
	    if ((gt = clicon_cli_genmodel_type(h)) == GT_ERR)
		goto done;
	    xml2cli(stdout, xc, NULL, gt); /* cli syntax */
	}
	break;
    case FORMAT_NETCONF:
	fprintf(stdout, "<rpc><edit-config><target><candidate/></target><config>\n");
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    clicon_xml2file(stdout, xc, 2, 1);
	fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
	break;
    }
    retval = 0;
done:
    if (xt)
	xml_free(xt);
    if (val)
	free(val);
    if (cbxpath)
	cbuf_free(cbxpath);
    return retval;
}

/*! Show configuration as text given an xpath
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath>
 * @note Hardcoded that a variable in cvv is named "xpath"
 */
int
show_conf_xpath(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    int              retval = -1;
    char            *str;
    char            *xpath;
    cg_var          *cv;
    cxobj           *xt = NULL;
    cxobj           *xerr;
    cxobj          **xv = NULL;
    size_t           xlen;
    int              i;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "%s: Requires one element to be <dbname>", __FUNCTION__);
	goto done;
    }
    str = cv_string_get(cvec_i(argv, 0));
    /* Dont get attr here, take it from arg instead */
    if (strcmp(str, "running") != 0 && 
	strcmp(str, "candidate") != 0 && 
	strcmp(str, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", str);	
	goto done;
    }
    cv = cvec_find_var(cvv, "xpath");
    xpath = cv_string_get(cv);
    if (clicon_rpc_get_config(h, str, xpath, &xt) < 0)
    	goto done;
    if ((xerr = xpath_first(xt, "/rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr);
	goto done;
    }
    if (xpath_vec(xt, xpath, &xv, &xlen) < 0) 
	goto done;
    for (i=0; i<xlen; i++)
	xml_print(stdout, xv[i]);

    retval = 0;
done:
    if (xv)
	free(xv);
    if (xt)
	xml_free(xt);
    return retval;
}
int show_confv_xpath(clicon_handle h, cvec *vars, cvec *argv)
{
    return show_conf_xpath(h, vars, argv);
}
