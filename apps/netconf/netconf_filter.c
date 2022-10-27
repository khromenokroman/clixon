/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC (Netgate)

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
 *  netconf match & selection: get and edit operations
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "netconf_rpc.h"
#include "netconf_filter.h"

/* xf specifices a filter, and xn is an xml tree.
 * Select the part of xn that matches xf and return it.
 * Change xn destructively by removing the parts of the sub-tree that does 
 * not match.
 * Match according to Section 6 of RFC 4741.
    NO_FILTER,       select all 
    EMPTY_FILTER,    select nothing 
    ATTRIBUTE_MATCH, select if attribute match 
    SELECTION,       select this node 
    CONTENT_MATCH,   select all siblings with matching content 
    CONTAINMENT      select 
 */

/* return a string containing leafs value, NULL if no leaf or no value */
static char*
leafstring(cxobj *x)
{
    cxobj *c;

    if (xml_type(x) != CX_ELMNT)
        return NULL;
    if (xml_child_nr(x) != 1)
        return NULL;
    c = xml_child_i(x, 0);
    if (xml_child_nr(c) != 0)
        return NULL;
    if (xml_type(c) != CX_BODY)
        return NULL;
    return xml_value(c);
}

/*! Internal recursive part where configuration xml tree is pruned from filter
 * assume parent has been selected and filter match (same name) as parent
 * parent is pruned according to selection.
 * @param[in]  xfilter  Filter xml
 * @param[out] xconf    Configuration xml
 * @retval  0  OK
 * @retval -1  Error
 */
static int
xml_filter_recursive(cxobj *xfilter, 
                     cxobj *xparent, 
                     int   *remove_me)
{
    cxobj *s;
    cxobj *sprev;
    cxobj *f;
    cxobj *attr;
    char *an;
    char *af;
    char *fstr;
    char *sstr;
    int   containments;
    int   remove_s;

    *remove_me = 0;
    /* 1. Check selection */
    if (xml_child_nr(xfilter) == 0) 
        goto match;

    /* Count containment/selection nodes in filter */
    f = NULL;
    containments = 0;
    while ((f = xml_child_each(xfilter, f, CX_ELMNT)) != NULL) {
        if (leafstring(f))
            continue;
        containments++;
    }

    /* 2. Check attribute match */
    attr = NULL;
    while ((attr = xml_child_each(xfilter, attr, CX_ATTR)) != NULL) {
        af = xml_value(attr);
        an = xml_find_value(xfilter, xml_name(attr));
        if (af && an && strcmp(af, an)==0)
            ; // match
        else
            goto nomatch;
    }
    /* 3. Check content match */
    f = NULL;
    while ((f = xml_child_each(xfilter, f, CX_ELMNT)) != NULL) {
        if ((fstr = leafstring(f)) == NULL)
            continue;
        if ((s = xml_find(xparent, xml_name(f))) == NULL)
            goto nomatch;
        if ((sstr = leafstring(s)) == NULL)
            continue;
        if (strcmp(fstr, sstr))
            goto nomatch;
    }
    /* If filter has no further specifiers, accept */
    if (!containments)
        goto match;
    /* Check recursively the rest of the siblings */
    sprev = s = NULL;
    while ((s = xml_child_each(xparent, s, CX_ELMNT)) != NULL) {
        if ((f = xml_find(xfilter, xml_name(s))) == NULL){
            xml_purge(s);
            s = sprev;
            continue;
        }
        if (leafstring(f)){
            sprev = s;
            continue; // unsure?sk=lf
        }
        // XXX: s can be removed itself in the recursive call !
        remove_s = 0;
        if (xml_filter_recursive(f, s, &remove_s) < 0)
            return -1;
        if (remove_s){
            xml_purge(s);
            s = sprev;
        }
        sprev = s;
    }

  match:
    return 0;
  nomatch: /* prune this parent node (maybe only children?) */
    *remove_me = 1;
    return 0;
}

/*! Remove parts of configuration xml tree that does not match filter xml tree
 * @param[in]  xfilter  Filter xml
 * @param[out] xconf    Configuration xml
 * @retval  0  OK
 * @retval -1  Error
 * This is the top-level function, calls a recursive variant.
 */
int
xml_filter(cxobj *xfilter, 
           cxobj *xconfig)
{
    int retval;
    int remove_s;

    /* Call recursive variant */
    retval = xml_filter_recursive(xfilter, 
                                  xconfig, 
                                  &remove_s);
    return retval;
}

