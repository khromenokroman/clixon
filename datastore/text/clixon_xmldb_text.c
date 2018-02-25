/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

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
1000 entries
valgrind --tool=callgrind datastore_client -d candidate -b /tmp/text -p ../datastore/text/text.so -y /tmp -m ietf-ip mget 300 /x/y[a=574][b=574] > /dev/null
  xml_copy_marked 87% 200x 
    yang_key_match 81% 600K
      yang_arg2cvec 52% 400K
      cvecfree      23% 400K

10000 entries
valgrind --tool=callgrind datastore_client -d candidate -b /tmp/text -p ../datastore/text/text.so -y /tmp -m ietf-ip mget 10 /x/y[a=574][b=574] > /dev/null

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <syslog.h>       
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_xmldb_text.h"

#define handle(xh) (assert(text_handle_check(xh)==0),(struct text_handle *)(xh))

/* Magic to ensure plugin sanity. */
#define TEXT_HANDLE_MAGIC 0x7f54da29

/*! Internal structure of text datastore handle. 
 */
struct text_handle {
    int            th_magic;    /* magic */
    char          *th_dbdir;    /* Directory of database files */
    yang_spec     *th_yangspec; /* Yang spec if this datastore */
    clicon_hash_t *th_dbs;      /* Hash of db_elements. key is dbname */
    int            th_cache;    /* Keep datastore text in memory so that get 
				   operation need only read memory.
				   Write to file on modification or file change.
				   Assumes single backend*/
    char          *th_format;   /* Datastroe format: xml / json */
    int            th_pretty;   /* Store xml/json pretty-printed. */
};

/* Struct per database in hash */
struct db_element{
    int    de_pid;
    cxobj *de_xml;
};

/*! Check struct magic number for sanity checks
 * return 0 if OK, -1 if fail.
 */
static int
text_handle_check(xmldb_handle xh)
{
    /* Dont use handle macro to avoid recursion */
    struct text_handle *th = (struct text_handle *)(xh);

    return th->th_magic == TEXT_HANDLE_MAGIC ? 0 : -1;
}

/*! Translate from symbolic database name to actual filename in file-system
 * @param[in]   th       text handle handle
 * @param[in]   db       Symbolic database name, eg "candidate", "running"
 * @param[out]  filename Filename. Unallocate after use with free()
 * @retval      0        OK
 * @retval     -1        Error
 * @note Could need a way to extend which databases exists, eg to register new.
 * The currently allowed databases are: 
 *   candidate, tmp, running, result
 * The filename reside in CLICON_XMLDB_DIR option
 */
static int
text_db2file(struct text_handle *th, 
	     const char         *db,
	     char              **filename)
{
    int   retval = -1;
    cbuf *cb;
    char *dir;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((dir = th->th_dbdir) == NULL){
	clicon_err(OE_XML, errno, "dbdir not set");
	goto done;
    }
    cprintf(cb, "%s/%s_db", dir, db);
    if ((*filename = strdup4(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Connect to a datastore plugin
 * @retval  handle  Use this handle for other API calls
 * @retval  NULL    Error
  */
xmldb_handle
text_connect(void)
{
    struct text_handle *th;
    xmldb_handle        xh = NULL;
    int                 size;

    size = sizeof(struct text_handle);
    if ((th = malloc(size)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(th, 0, size);
    th->th_magic = TEXT_HANDLE_MAGIC;
    th->th_format = "xml"; /* default */
    th->th_pretty = 1; /* default */
    th->th_cache = 1; /* default */
    if ((th->th_dbs = hash_init()) == NULL)
	goto done;
    xh = (xmldb_handle)th;
  done:
    return xh;

}

/*! Disconnect from to a datastore plugin and deallocate handle
 * @param[in]  xh      XMLDB handle, disconect and deallocate from this handle
 * @retval     0       OK
  */
int
text_disconnect(xmldb_handle xh)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);
    struct db_element  *de;
    char              **keys = NULL;
    size_t              klen;
    int                 i;
	
    if (th){
	if (th->th_dbdir)
	    free(th->th_dbdir);
	if (th->th_dbs){
	    if (th->th_cache){
		if ((keys = hash_keys(th->th_dbs, &klen)) == NULL)
		    return 0;
		for(i = 0; i < klen; i++) 
		    if ((de = hash_value(th->th_dbs, keys[i], NULL)) != NULL){
			if (de->de_xml)
			    xml_free(de->de_xml);
		    }
		if (keys)
		    free(keys);
	    }
	    hash_free(th->th_dbs);
	}
	free(th);
    }
    retval = 0;
    // done:
    return retval;
}

/*! Get value of generic plugin option. Type of value is givenby context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name
 * @param[out] value   Pointer to Value of option
 * @retval     0       OK
 * @retval    -1       Error
 */
int
text_getopt(xmldb_handle xh, 
	    char        *optname,
	    void       **value)
{
    int               retval = -1;
    struct text_handle *th = handle(xh);

    if (strcmp(optname, "yangspec") == 0)
	*value = th->th_yangspec;
    else if (strcmp(optname, "dbdir") == 0)
	*value = th->th_dbdir;
    else if (strcmp(optname, "xml_cache") == 0)
	*value = &th->th_cache;
    else if (strcmp(optname, "format") == 0)
	*value = th->th_format;
    else if (strcmp(optname, "pretty") == 0)
	*value = &th->th_pretty;
    else{
	clicon_err(OE_PLUGIN, 0, "Option %s not implemented by plugin", optname);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Set value of generic plugin option. Type of value is given by context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name: yangspec, xml_cache, format, prettyprint
 * @param[in]  value   Value of option
 * @retval     0       OK
 * @retval    -1       Error
 */
int
text_setopt(xmldb_handle xh,
	    char        *optname,
	    void        *value)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);

    if (strcmp(optname, "yangspec") == 0)
	th->th_yangspec = (yang_spec*)value;
    else if (strcmp(optname, "dbdir") == 0){
	if (value && (th->th_dbdir = strdup((char*)value)) == NULL){
	    clicon_err(OE_UNIX, 0, "strdup");
	    goto done;
	}
    }
    else if (strcmp(optname, "xml_cache") == 0){
	th->th_cache = (intptr_t)value;
    }
    else if (strcmp(optname, "format") == 0){
	if (strcmp(value,"xml")==0)
	    th->th_format = "xml";
	else if (strcmp(value,"json")==0)
	    th->th_format = "json";
	else{
	    clicon_err(OE_PLUGIN, 0, "Option %s unrecognized format: %s", optname, value);
	    goto done;
	}
    }
    else if (strcmp(optname, "pretty") == 0){
	th->th_pretty = (intptr_t)value;
    }
    else{
	clicon_err(OE_PLUGIN, 0, "Option %s not implemented by plugin", optname);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Ensure that xt only has a single sub-element and that is "config" 
 */
static int
singleconfigroot(cxobj  *xt, 
		 cxobj **xp)
{
    int    retval = -1;
    cxobj *x = NULL;
    int    i = 0;

    /* There should only be one element and called config */
    x = NULL;
    while ((x = xml_child_each(xt, x,  CX_ELMNT)) != NULL){
	i++;
	if (strcmp(xml_name(x), "config")){
	    clicon_err(OE_DB, ENOENT, "Wrong top-element %s expected config", 
		       xml_name(x));
	    goto done;
	}
    }
    if (i != 1){
	clicon_err(OE_DB, ENOENT, "Top-element is not unique, expecting single  config");
	goto done;
    }
    x = NULL;
    while ((x = xml_child_each(xt, x,  CX_ELMNT)) != NULL){
	if (xml_rm(x) < 0)
	    goto done;
	if (xml_free(xt) < 0)
	    goto done;
	*xp = x;
	break;
    }
    retval = 0;
 done:
    return retval;
}

/*! Given XML tree x0 with marked nodes, copy marked nodes to new tree x1
 * Two marks are used: XML_FLAG_MARK and XML_FLAG_CHANGE
 *
 * The algorithm works as following:
 * (1) Copy individual nodes marked with XML_FLAG_CHANGE 
 * until nodes marked with XML_FLAG_MARK are reached, where 
 * (2) the complete subtree of that node is copied. 
 * (3) Special case: key nodes in lists are copied if any node in list is marked
 */
static int
xml_copy_marked(cxobj *x0, 
		cxobj *x1)
{
    int        retval = -1;
    int        mark;
    cxobj     *x;
    cxobj     *xcopy;
    int        iskey;
    yang_stmt *yt;
    char      *name;

    assert(x0 && x1);
    yt = xml_spec(x0); /* can be null */
    /* Go through children to detect any marked nodes:
     * (3) Special case: key nodes in lists are copied if any 
     * node in list is marked
     */
    mark = 0;
    x = NULL;
    while ((x = xml_child_each(x0, x, CX_ELMNT)) != NULL) {
	if (xml_flag(x, XML_FLAG_MARK|XML_FLAG_CHANGE)){
	    mark++;
	    break;
	}
    }
    x = NULL;
    while ((x = xml_child_each(x0, x, CX_ELMNT)) != NULL) {
	name = xml_name(x);
	if (xml_flag(x, XML_FLAG_MARK)){
	    /* (2) the complete subtree of that node is copied. */
	    if ((xcopy = xml_new(name, x1, xml_spec(x))) == NULL)
		goto done;
	    if (xml_copy(x, xcopy) < 0) 
		goto done;
	    continue; 
	}
	if (xml_flag(x, XML_FLAG_CHANGE)){
	    /*  Copy individual nodes marked with XML_FLAG_CHANGE */
	    if ((xcopy = xml_new(name, x1, xml_spec(x))) == NULL)
		goto done;
	    if (xml_copy_marked(x, xcopy) < 0) /*  */
		goto done;
	}
	/* (3) Special case: key nodes in lists are copied if any 
	 * node in list is marked */
	if (mark && yt && yt->ys_keyword == Y_LIST){
	    /* XXX: I think yang_key_match is suboptimal here */
	    if ((iskey = yang_key_match((yang_node*)yt, name)) < 0)
		goto done;
	    if (iskey){
		if ((xcopy = xml_new(name, x1, xml_spec(x))) == NULL)
		    goto done;
		if (xml_copy(x, xcopy) < 0) 
		    goto done;
	    }
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Get content of database using xpath. return a set of matching sub-trees
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath.
 * This is a clixon datastore plugin of the the xmldb api
 * @see xmldb_get
 */
int
text_get(xmldb_handle xh,
	 const char   *db, 
	 char         *xpath,
	 int           config,
	 cxobj       **xtop)
{
    int             retval = -1;
    char           *dbfile = NULL;
    yang_spec      *yspec;
    cxobj          *xt = NULL;
    int             fd = -1;
    cxobj         **xvec = NULL;
    size_t          xlen;
    int             i;
    struct text_handle *th = handle(xh);
    struct db_element *de = NULL;

    if ((yspec = th->th_yangspec) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if (th->th_cache){
	if ((de = hash_value(th->th_dbs, db, NULL)) != NULL)
	    xt = de->de_xml; 
    }
    if (xt == NULL){
	if (text_db2file(th, db, &dbfile) < 0)
	    goto done;
	if (dbfile==NULL){
	    clicon_err(OE_XML, 0, "dbfile NULL");
	    goto done;
	}
	if ((fd = open(dbfile, O_RDONLY)) < 0){
	    clicon_err(OE_UNIX, errno, "open(%s)", dbfile);
	    goto done;
	}    
	/* Parse file into XML tree */
	if (strcmp(th->th_format,"json")==0){
	    if ((json_parse_file(fd, yspec, &xt)) < 0)
		goto done;
	}
	else if ((xml_parse_file(fd, "</config>", yspec, &xt)) < 0)
	    goto done;
	/* Always assert a top-level called "config". 
	   To ensure that, deal with two cases:
	   1. File is empty <top/> -> rename top-level to "config" */
	if (xml_child_nr(xt) == 0){ 
	    if (xml_name_set(xt, "config") < 0)
		goto done;     
	}
	/* 2. File is not empty <top><config>...</config></top> -> replace root */
	else{ 
	    /* There should only be one element and called config */
	    if (singleconfigroot(xt, &xt) < 0)
		goto done;
	}
    } /* xt == NULL */
    /* Here xt looks like: <config>...</config> */

    if (xpath_vec(xt, xpath?xpath:"/", &xvec, &xlen) < 0)
	goto done;

    /* If vectors are specified then mark the nodes found with all ancestors
     * and filter out everything else,
     * otherwise return complete tree.
     */
    if (xvec != NULL)
	for (i=0; i<xlen; i++){
	    xml_flag_set(xvec[i], XML_FLAG_MARK);
	    if (th->th_cache)
		xml_apply_ancestor(xvec[i], (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
	}

    if (th->th_cache){
	/* Copy the matching parts of the (relevant) XML tree.
	 * If cache was NULL, also write to datastore cache
	 */
	cxobj *x1;
	struct db_element de0 = {0,};

	if (de != NULL)
	    de0 = *de;

	x1 = xml_new(xml_name(xt), NULL, xml_spec(xt));
	/* Copy everything that is marked */
	if (xml_copy_marked(xt, x1) < 0)
	    goto done;
	if (xml_apply(xt, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE)) < 0)
	    goto done;
	if (xml_apply(x1, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE)) < 0)
	    goto done;
	if (de0.de_xml == NULL){
	    de0.de_xml = xt;
	    hash_add(th->th_dbs, db, &de0, sizeof(de0));
	}
	xt = x1;
    }
    else{
	/* Remove everything that is not marked */
	if (!xml_flag(xt, XML_FLAG_MARK))
	    if (xml_tree_prune_flagged_sub(xt, XML_FLAG_MARK, 1, NULL) < 0)
		goto done;
    }
    /* reset flag */
    if (xml_apply(xt, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)XML_FLAG_MARK) < 0)
	goto done;
    /* filter out state (operations) data if config not set. Mark all nodes
     that are not config data */
    if (config && xml_apply(xt, CX_ELMNT, xml_non_config_data, NULL) < 0)
	goto done;
    /* Remove (prune) nodes that are marked (that does not pass test) */
    if (xml_tree_prune_flagged(xt, XML_FLAG_MARK, 1) < 0)
	goto done;
    /* Add default values (if not set) */
    if (xml_apply(xt, CX_ELMNT, xml_default, NULL) < 0)
	goto done;
    /* Order XML children according to YANG */
    if (!xml_child_sort && xml_apply(xt, CX_ELMNT, xml_order, NULL) < 0)
	goto done;
#if 0 /* debug */
    if (xml_child_sort && xml_apply0(xt, -1, xml_sort_verify, NULL) < 0)
	clicon_log(LOG_NOTICE, "%s: verify failed #2", __FUNCTION__);
#endif
    if (debug>1)
    	clicon_xml2file(stderr, xt, 0, 1);
    *xtop = xt;
    xt = NULL;
    retval = 0;
 done:
    if (xt)
	xml_free(xt);
    if (dbfile)
	free(dbfile);
    if (xvec)
	free(xvec);
    if (fd != -1)
	close(fd);
    return retval;
}

/*! Modify a base tree x0 with x1 with yang spec y according to operation op
 * @param[in]  x0  Base xml tree (can be NULL in add scenarios)
 * @param[in]  y0  Yang spec corresponding to xml-node x0. NULL if x0 is NULL
 * @param[in]  x0p Parent of x0
 * @param[in]  x1  xml tree which modifies base
 * @param[in]  op  OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * Assume x0 and x1 are same on entry and that y is the spec
 * @see put in clixon_keyvalue.c
 */
static int
text_modify(cxobj              *x0,
	    yang_node          *y0,
	    cxobj              *x0p,
	    cxobj              *x1,
	    enum operation_type op)
{
    int        retval = -1;
    char      *opstr;
    char      *x1name;
    char      *x1cname; /* child name */
    cxobj     *x0c; /* base child */
    cxobj     *x0b; /* base body */
    cxobj     *x1c; /* mod child */
    char      *x1bstr; /* mod body string */
    yang_stmt *yc;  /* yang child */
    cxobj    **x0vec = NULL;
    int        i;

    assert(x1 && xml_type(x1) == CX_ELMNT);
    assert(y0);
    /* Check for operations embedded in tree according to netconf */
    if ((opstr = xml_find_value(x1, "operation")) != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    x1name = xml_name(x1);
    if (y0->yn_keyword == Y_LEAF_LIST || y0->yn_keyword == Y_LEAF){
	x1bstr = xml_body(x1);
	switch(op){ 
	case OP_CREATE:
	    if (x0){
		clicon_err(OE_XML, 0, "Object to create already exists");
		goto done;
	    }
	case OP_NONE: /* fall thru */
	case OP_MERGE:
	case OP_REPLACE:
	    if (x0==NULL){
		//		int iamkey=0;
		if ((x0 = xml_new(x1name, x0p, (yang_stmt*)y0)) == NULL)
		    goto done;
#if 0
		/* If it is key I dont want to mark it */
		if ((iamkey=yang_key_match(y0->yn_parent, x1name)) < 0)
		    goto done;
		if (!iamkey && op==OP_NONE)
#else
		if (op==OP_NONE)
#endif
		    xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
		if (x1bstr){ /* empty type does not have body */
		    if ((x0b = xml_new("body", x0, NULL)) == NULL)
			goto done; 
		    xml_type_set(x0b, CX_BODY);
		}
	    }
	    if (x1bstr){
		if ((x0b = xml_body_get(x0)) == NULL){
		    if ((x0b = xml_new("body", x0, NULL)) == NULL)
			goto done; 
		    xml_type_set(x0b, CX_BODY);
		}
		if (xml_value_set(x0b, x1bstr) < 0)
		    goto done;
	    }
	    break;
	case OP_DELETE:
	    if (x0==NULL){
		clicon_err(OE_XML, 0, "Object to delete does not exist");
		goto done;
	    }
	case OP_REMOVE: /* fall thru */
	    if (x0){
		xml_purge(x0);
	    }
	    break;
	default:
	    break;
	} /* switch op */
    } /* if LEAF|LEAF_LIST */
    else { /* eg Y_CONTAINER, Y_LIST, Y_ANYXML  */
	switch(op){ 
	case OP_CREATE:
	    if (x0){
		clicon_err(OE_XML, 0, "Object to create already exists");
		goto done;
	    }
	case OP_REPLACE: /* fall thru */
	    if (x0){
		xml_purge(x0);
		x0 = NULL;
	    }
	case OP_MERGE:  /* fall thru */
	case OP_NONE: 
	    /* Special case: anyxml, just replace tree, 
	       See 7.10.3 of RFC6020bis */
	    if (y0->yn_keyword == Y_ANYXML){
		if (op == OP_NONE)
		    break;
		if (x0){
		    xml_purge(x0);
		}
		if ((x0 = xml_new(x1name, x0p, (yang_stmt*)y0)) == NULL)
		    goto done;
		if (xml_copy(x1, x0) < 0)
		    goto done;
		break;
	    }
	    if (x0==NULL){
		if ((x0 = xml_new(x1name, x0p, (yang_stmt*)y0)) == NULL)
		    goto done;
		if (op==OP_NONE)
		    xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
	    }
	    /* First pass: mark existing children in base */
	    /* Loop through children of the modification tree */
	    if ((x0vec = calloc(xml_child_nr(x1), sizeof(x1))) == NULL){
		clicon_err(OE_UNIX, errno, "calloc");
		goto done;
	    }
	    x1c = NULL; 
	    i = 0;
	    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
		x1cname = xml_name(x1c);
		/* Get yang spec of the child */
		if ((yc = yang_find_datanode(y0, x1cname)) == NULL){
		    clicon_err(OE_YANG, errno, "No yang node found: %s", x1cname);
		    goto done;
		}
		/* See if there is a corresponding node in the base tree */
		x0c = NULL;
		if (match_base_child(x0, x1c, &x0c, yc) < 0)
		    goto done;
		x0vec[i++] = x0c;
	    }
	    /* Second pass: modify tree */
	    x1c = NULL;
	    i = 0;
	    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
		x1cname = xml_name(x1c);
		yc = yang_find_datanode(y0, x1cname);
		if (text_modify(x0vec[i++], (yang_node*)yc, x0, x1c, op) < 0)
		    goto done;
	    }
	    break;
	case OP_DELETE:
	    if (x0==NULL){
		clicon_err(OE_XML, 0, "Object to delete does not exist");
		goto done;
	    }
	case OP_REMOVE: /* fall thru */
	    if (x0)
		xml_purge(x0);
	    break;
	default:
	    break;
	} /* CONTAINER switch op */
    } /* else Y_CONTAINER  */
    // ok:
    xml_sort(x0p, NULL);
    retval = 0;
 done:
    if (x0vec)
	free(x0vec);
    return retval;
}

/*! Modify a top-level base tree x0 with modification tree x1
 * @param[in]  x0  Base xml tree (can be NULL in add scenarios)
 * @param[in]  x1  xml tree which modifies base
 * @param[in]  yspec Top-level yang spec (if y is NULL)
 * @param[in]  op  OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @see text_modify
 */
static int
text_modify_top(cxobj              *x0,
		cxobj              *x1,
		yang_spec          *yspec,
		enum operation_type op)
{
    int        retval = -1;
    char      *x1cname; /* child name */
    cxobj     *x0c; /* base child */
    cxobj     *x1c; /* mod child */
    yang_stmt *yc;  /* yang child */
    char      *opstr;

    /* Assure top-levels are 'config' */
    assert(x0 && strcmp(xml_name(x0),"config")==0);
    assert(x1 && strcmp(xml_name(x1),"config")==0);

    /* Check for operations embedded in tree according to netconf */
    if ((opstr = xml_find_value(x1, "operation")) != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    /* Special case if x1 is empty, top-level only <config/> */
    if (!xml_child_nr(x1)){ 
	if (xml_child_nr(x0)) /* base tree not empty */
	    switch(op){ 
	    case OP_DELETE:
	    case OP_REMOVE:
	    case OP_REPLACE:
		x0c = NULL;
		while ((x0c = xml_child_each(x0, x0c, CX_ELMNT)) != NULL) 
		    xml_purge(x0c);
		break;
	    default:
		break;
	    }
	else /* base tree empty */
	    switch(op){ 
	    case OP_DELETE:
		clicon_err(OE_XML, 0, "Object to delete does not exist");
		break;
	    default:
		break;
	    }
    }
    /* Special case top-level replace */
    if (op == OP_REPLACE || op == OP_DELETE){
	x0c = NULL;
	while ((x0c = xml_child_each(x0, x0c, CX_ELMNT)) != NULL) 
	    xml_purge(x0c);
    }
    /* Loop through children of the modification tree */
    x1c = NULL;
    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
	x1cname = xml_name(x1c);
	/* Get yang spec of the child */
	if ((yc = yang_find_topnode(yspec, x1cname, 0)) == NULL){
	    clicon_err(OE_YANG, ENOENT, "No yang spec");
	    goto done;
	}
	/* See if there is a corresponding node in the base tree */
	if (match_base_child(x0, x1c, &x0c, yc) < 0)
	    goto done;
	if (text_modify(x0c, (yang_node*)yc, x0, x1c, op) < 0)
	    goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! For containers without presence and no children, remove
 * @param[in]   x       XML tree node
 * See section 7.5.1 in rfc6020bis-02.txt:
 * No presence:
 * those that exist only for organizing the hierarchy of data nodes:
 * the container has no meaning of its own, existing
 * only to contain child nodes.  This is the default style.
 * (Remove these if no children)
 * Presence:
 * the presence of the container itself is
 * configuration data, representing a single bit of configuration data.
 * The container acts as both a configuration knob and a means of
 * organizing related configuration.  These containers are explicitly
 * created and deleted.
 * (Dont touch these)
 */
int
xml_container_presence(cxobj  *x, 
		       void   *arg)
{
    int        retval = -1;
    yang_stmt *y;  /* yang node */

    if ((y = (yang_stmt*)xml_spec(x)) == NULL){
	retval = 0;
	goto done;
    }
    /* Mark node that is: container, have no children, dont have presence */
    if (y->ys_keyword == Y_CONTAINER && 
	xml_child_nr(x)==0 &&
	yang_find((yang_node*)y, Y_PRESENCE, NULL) == NULL)
	xml_flag_set(x, XML_FLAG_MARK); /* Mark, remove later */
    retval = 0;
 done:
    return retval;
}

/*! Modify database provided an xml tree and an operation
 * This is a clixon datastore plugin of the the xmldb api
 * @see xmldb_put
 */
int
text_put(xmldb_handle        xh,
	 const char         *db, 
	 enum operation_type op,
	 cxobj              *x1)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *dbfile = NULL;
    int                 fd = -1;
    FILE               *f = NULL;
    cbuf               *cb = NULL;
    yang_spec          *yspec;
    cxobj              *x0 = NULL;
    struct db_element  *de = NULL;
    
    if ((yspec =  th->th_yangspec) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if (x1 && strcmp(xml_name(x1),"config")!=0){
	clicon_err(OE_XML, 0, "Top-level symbol of modification tree is %s, expected \"config\"",
		   xml_name(x1));
	goto done;
    }
    if (th->th_cache){
	if ((de = hash_value(th->th_dbs, db, NULL)) != NULL)
	    x0 = de->de_xml; 
    }
    if (x0 == NULL){
	if (text_db2file(th, db, &dbfile) < 0)
	    goto done;
	if (dbfile==NULL){
	    clicon_err(OE_XML, 0, "dbfile NULL");
	    goto done;
	}
	if ((fd = open(dbfile, O_RDONLY)) < 0) {
	    clicon_err(OE_UNIX, errno, "open(%s)", dbfile);
	    goto done;
	}    
	/* Parse file into XML tree */
	if (strcmp(th->th_format,"json")==0){
	    if ((json_parse_file(fd, yspec, &x0)) < 0)
		goto done;
	}
	else if ((xml_parse_file(fd, "</config>", yspec, &x0)) < 0)
	    goto done;
	/* Always assert a top-level called "config". 
	   To ensure that, deal with two cases:
	   1. File is empty <top/> -> rename top-level to "config" */
	if (xml_child_nr(x0) == 0){ 
	    if (xml_name_set(x0, "config") < 0)
		goto done;     
	}
	/* 2. File is not empty <top><config>...</config></top> -> replace root */
	else{ 
	    /* There should only be one element and called config */
	    if (singleconfigroot(x0, &x0) < 0)
		goto done;
	}
    }
    /* Here x0 looks like: <config>...</config> */
    if (strcmp(xml_name(x0),"config")!=0){
	clicon_err(OE_XML, 0, "Top-level symbol is %s, expected \"config\"",
		   xml_name(x0));
	goto done;
    }

    /* Add yang specification backpointer to all XML nodes */
    /* XXX: where is this created? Add yspec */
    if (xml_apply(x1, CX_ELMNT, xml_spec_populate, yspec) < 0)
       goto done;
#if 0 /* debug */
    if (xml_child_sort && xml_apply0(x1, -1, xml_sort_verify, NULL) < 0)
	clicon_log(LOG_NOTICE, "%s: verify failed #1", __FUNCTION__);
#endif
    /* 
     * Modify base tree x with modification x1. This is where the
     * new tree is made.
     */
    if (text_modify_top(x0, x1, yspec, op) < 0)
	goto done;

    /* Remove NONE nodes if all subs recursively are also NONE */
    if (xml_tree_prune_flagged_sub(x0, XML_FLAG_NONE, 0, NULL) <0)
	goto done;
    if (xml_apply(x0, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, 
		  (void*)XML_FLAG_NONE) < 0)
	goto done;
    /* Mark non-presence containers that do not have children */
    if (xml_apply(x0, CX_ELMNT, (xml_applyfn_t*)xml_container_presence, NULL) < 0)
	goto done;
    /* Remove (prune) nodes that are marked (non-presence containers w/o children) */
    if (xml_tree_prune_flagged(x0, XML_FLAG_MARK, 1) < 0)
	goto done;
#if 0 /* debug */
    if (xml_child_sort && xml_apply0(x0, -1, xml_sort_verify, NULL) < 0)
	clicon_log(LOG_NOTICE, "%s: verify failed #3", __FUNCTION__);
#endif
    /* Write back to datastore cache if first time */
    if (th->th_cache){
	struct db_element de0 = {0,};
	if (de != NULL)
	    de0 = *de;
	if (de0.de_xml == NULL){
	    de0.de_xml = x0;
	    hash_add(th->th_dbs, db, &de0, sizeof(de0));
	}
    }
    if (dbfile == NULL){
	if (text_db2file(th, db, &dbfile) < 0)
	    goto done;
	if (dbfile==NULL){
	    clicon_err(OE_XML, 0, "dbfile NULL");
	    goto done;
	}
    }
    if (fd != -1){
	close(fd);
	fd = -1;
    }
    if ((f = fopen(dbfile, "w")) == NULL){
	clicon_err(OE_CFG, errno, "Creating file %s", dbfile);
	goto done;
    } 
    if (strcmp(th->th_format,"json")==0){
	if (xml2json(f, x0, th->th_pretty) < 0)
	    goto done;
    }
    else if (clicon_xml2file(f, x0, 0, th->th_pretty) < 0)
	goto done;
    retval = 0;
 done:
    if (f != NULL)
	fclose(f);
    if (dbfile)
	free(dbfile);
    if (fd != -1)
	close(fd);
    if (cb)
	cbuf_free(cb);
    if (!th->th_cache && x0)
	xml_free(x0);
    return retval;
}

/*! Copy database from db1 to db2
 * @param[in]  xh  XMLDB handle
 * @param[in]  from  Source database
 * @param[in]  to    Destination database
 * @retval -1  Error
 * @retval  0  OK
  */
int 
text_copy(xmldb_handle xh, 
	  const char  *from,
	  const char  *to)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *fromfile = NULL;
    char               *tofile = NULL;
    struct db_element  *de = NULL;
    struct db_element  *de2 = NULL;

    /* XXX lock */
    if (th->th_cache){
	/* 1. Free xml tree in "to"
	 */
	if ((de = hash_value(th->th_dbs, to, NULL)) != NULL){
	    if (de->de_xml != NULL){
		xml_free(de->de_xml);
		de->de_xml = NULL;
	    }
	}
	/* 2. Copy xml tree from "from" to "to" 
	 * 2a) create "to" if it does not exist
	 */
	if ((de2 = hash_value(th->th_dbs, from, NULL)) != NULL){
	    if (de2->de_xml != NULL){
		struct db_element de0 = {0,};
		cxobj *x, *xcopy;
		x = de2->de_xml;
		if (de != NULL)
		    de0 = *de;
		if ((xcopy = xml_new(xml_name(x), NULL, xml_spec(x))) == NULL)
		    goto done;
		if (xml_copy(x, xcopy) < 0) 
		    goto done;
		de0.de_xml = xcopy;
		hash_add(th->th_dbs, to, &de0, sizeof(de0));
	    }
	}
    }
    if (text_db2file(th, from, &fromfile) < 0)
	goto done;
    if (text_db2file(th, to, &tofile) < 0)
	goto done;
    if (clicon_file_copy(fromfile, tofile) < 0)
	goto done;
    retval = 0;
 done:
    if (fromfile)
	free(fromfile);
    if (tofile)
	free(tofile);
    return retval;
}

/*! Lock database
 * @param[in]  xh   XMLDB handle
 * @param[in]  db   Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
  */
int 
text_lock(xmldb_handle xh, 
	  const char  *db,
	  int          pid)
{
    struct text_handle *th = handle(xh);
    struct db_element  *de = NULL;
    struct db_element   de0 = {0,};

    if ((de = hash_value(th->th_dbs, db, NULL)) != NULL)
	de0 = *de;
    de0.de_pid = pid;
    hash_add(th->th_dbs, db, &de0, sizeof(de0));
    clicon_debug(1, "%s: locked by %u",  db, pid);
    return 0;
}

/*! Unlock database
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
 * Assume all sanity checks have been made
 */
int 
text_unlock(xmldb_handle xh, 
	    const char  *db)
{
    struct text_handle *th = handle(xh);
    struct db_element  *de = NULL;

    if ((de = hash_value(th->th_dbs, db, NULL)) != NULL){
	de->de_pid = 0;
	hash_add(th->th_dbs, db, de, sizeof(*de));
    }
    return 0;
}

/*! Unlock all databases locked by pid (eg process dies) 
 * @param[in]    xh  XMLDB handle
 * @param[in]    pid Process / Session id
 * @retval -1    Error
 * @retval   0   Ok
 */
int 
text_unlock_all(xmldb_handle xh, 
	      int            pid)
{
    struct text_handle *th = handle(xh);
    char              **keys = NULL;
    size_t              klen;
    int                 i;
    struct db_element  *de;

    if ((keys = hash_keys(th->th_dbs, &klen)) == NULL)
	return 0;
    for(i = 0; i < klen; i++) 
	if ((de = hash_value(th->th_dbs, keys[i], NULL)) != NULL &&
	    de->de_pid == pid){
	    de->de_pid = 0;
	    hash_add(th->th_dbs, keys[i], de, sizeof(*de));
	}
    if (keys)
	free(keys);
    return 0;
}

/*! Check if database is locked
 * @param[in]    xh  XMLDB handle
 * @param[in]    db  Database
 * @retval -1    Error
 * @retval   0   Not locked
 * @retval  >0   Id of locker
  */
int 
text_islocked(xmldb_handle xh, 
	      const char  *db)
{
    struct text_handle *th = handle(xh);
    struct db_element  *de;

    if ((de = hash_value(th->th_dbs, db, NULL)) == NULL)
	return 0;
    return de->de_pid;
}

/*! Check if db exists 
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  No it does not exist
 * @retval  1  Yes it exists
 */
int 
text_exists(xmldb_handle  xh, 
	    const char   *db)
{

    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *filename = NULL;
    struct stat         sb;

    if (text_db2file(th, db, &filename) < 0)
	goto done;
    if (lstat(filename, &sb) < 0)
	retval = 0;
    else
	retval = 1;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! Delete database. Remove file 
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  OK
 */
int 
text_delete(xmldb_handle xh, 
	    const char  *db)
{
    int                 retval = -1;
    char               *filename = NULL;
    struct text_handle *th = handle(xh);
    struct db_element  *de = NULL;
    cxobj              *xt = NULL;
    struct stat         sb;
    
    if (th->th_cache){
	if ((de = hash_value(th->th_dbs, db, NULL)) != NULL){
	    if ((xt = de->de_xml) != NULL){
		xml_free(xt);
		de->de_xml = NULL;
	    }
	}
    }
    if (text_db2file(th, db, &filename) < 0)
	goto done;
    if (lstat(filename, &sb) == 0)
	if (unlink(filename) < 0){
	    clicon_err(OE_DB, errno, "unlink %s", filename);
	    goto done;
	}
    retval = 0;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! Create / init database 
 * If it exists dont change.
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @retval  0  OK
 * @retval -1  Error
 */
int 
text_create(xmldb_handle xh, 
	    const char  *db)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *filename = NULL;
    int                 fd = -1;
    struct db_element  *de = NULL;
    cxobj              *xt = NULL;

    if (th->th_cache){ /* XXX This should nt really happen? */
	if ((de = hash_value(th->th_dbs, db, NULL)) != NULL){
	    if ((xt = de->de_xml) != NULL){
		assert(xt==NULL); /* XXX */
		xml_free(xt);
		de->de_xml = NULL;
	    }
	}
    }
    if (text_db2file(th, db, &filename) < 0)
	goto done;
    if ((fd = open(filename, O_CREAT|O_WRONLY, S_IRWXU)) == -1) {
	clicon_err(OE_UNIX, errno, "open(%s)", filename);
	goto done;
    }
   retval = 0;
 done:
    if (filename)
	free(filename);
    if (fd != -1)
	close(fd);
    return retval;
}

/*! plugin exit function */
int
text_plugin_exit(void)
{
    return 0;
}

static const struct xmldb_api api;
static const struct xmldb_api api;

/*! plugin init function */
void *
clixon_xmldb_plugin_init(int version)
{
    if (version != XMLDB_API_VERSION){
	clicon_err(OE_DB, 0, "Invalid version %d expected %d", 
		   version, XMLDB_API_VERSION);
	goto done;
    }
    return (void*)&api;
 done:
    return NULL;
}

static const struct xmldb_api api = {
    1,
    XMLDB_API_MAGIC,
    clixon_xmldb_plugin_init,
    text_plugin_exit,
    text_connect,
    text_disconnect,
    text_getopt,
    text_setopt,
    text_get,
    text_put,
    text_copy,
    text_lock,
    text_unlock,
    text_unlock_all,
    text_islocked,
    text_exists,
    text_delete,
    text_create,
};


#if 0 /* Test program */
/*
 * Turn this on to get an xpath test program 
 * Usage: clicon_xpath [<xpath>] 
 * read xml from input
 * Example compile:
 gcc -g -o xmldb -I. -I../clixon ./clixon_xmldb.c -lclixon -lcligen
*/

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:\n%s\tget <db> <yangdir> <yangmod> [<xpath>]\t\txml on stdin\n", argv0);
    fprintf(stderr, "\tput <db> <yangdir> <yangmod> set|merge|delete\txml to stdout\n");
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    cxobj      *xt;
    cxobj      *xn;
    char       *xpath;
    enum operation_type      op;
    char       *cmd;
    char       *db;
    char       *yangdir;
    char       *yangmod;
    yang_spec  *yspec = NULL;
    clicon_handle h;

    if ((h = clicon_handle_init()) == NULL)
	goto done;
    clicon_log_init("xmldb", LOG_DEBUG, CLICON_LOG_STDERR);
    if (argc < 4){
	usage(argv[0]);
	goto done;
    }
    cmd = argv[1];
    db = argv[2];
    yangdir = argv[3];
    yangmod = argv[4];
    db_init(db);
    if ((yspec = yspec_new()) == NULL)
	goto done
    if (yang_parse(h, yangdir, yangmod, NULL, yspec) < 0)
	goto done;
    if (strcmp(cmd, "get")==0){
	if (argc < 5)
	    usage(argv[0]);
	xpath = argc>5?argv[5]:NULL;
	if (xmldb_get(h, db, xpath, &xt, NULL, 1, NULL) < 0)
	    goto done;
	if (strcmp(th->th_format,"json")==0){
	    if (xml2json(stdout, xt, th->th_pretty) < 0)
		goto done;
	}
	else{
	    if (clicon_xml2file(stdout, xt, 0, th->th_pretty) < 0)
		goto done;
	}
    }
    else
    if (strcmp(cmd, "put")==0){
	if (argc != 6)
	    usage(argv[0]);
	if (xml_parse_file(0, "</clicon>", NULL, &xt) < 0)
	    goto done;
	if (xml_rootchild(xt, 0, &xn) < 0)
	    goto done;
	if (strcmp(argv[5], "set") == 0)
	    op = OP_REPLACE;
	else 	
	    if (strcmp(argv[4], "merge") == 0)
	    op = OP_MERGE;
	else 	if (strcmp(argv[5], "delete") == 0)
	    op = OP_REMOVE;
	else
	    usage(argv[0]);
	if (xmldb_put(h, db, op, NULL, xn) < 0)
	    goto done;
    }
    else
	usage(argv[0]);
    printf("\n");
 done:
    return 0;
}

#endif /* Test program */
