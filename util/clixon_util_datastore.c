/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Command line options to be passed to getopt(3) */
#define DATASTORE_OPTS "hDd:b:f:x:y:Y:"

/*! usage
 */
static void
usage(char *argv0)
{
    fprintf(stderr, "usage:%s <options>* [<command>]\n"
            "where options are\n"
            "\t-h\t\tHelp\n"
            "\t-D\t\tDebug\n"
            "\t-d <db>\t\tDatabase name. Default: running. Alt: candidate,startup\n"
            "\t-b <dir>\tDatabase directory. Mandatory\n"
            "\t-f <fmt>\tDatabase format: xml or json\n"
            "\t-x <xml>\tXML file. Alternative to put <xml> argument\n"
            "\t-y <file>\tYang file. Mandatory\n"
            "\t-Y <dir> \tYang dirs (can be several)\n"
            "and command is either:\n"
            "\tget [<xpath>]\n"
            "\tmget <nr> [<xpath>]\n"
            "\tput (merge|replace|create|delete|remove) [<xml>]\n"
            "\tcopy <todb>\n"
            "\tlock <pid>\n"
            "\tunlock\n"
            "\tunlock_all <pid>\n"
            "\tislocked\n"
            "\texists\n"
            "\tdelete\n"
            "\tinit\n"
            ,
            argv0
            );
    exit(0);
}

int
main(int argc, char **argv)
{
    int                 retval = -1;
    int                 c;
    clicon_handle       h;
    char               *argv0;
    char               *db = "running";
    char               *cmd = NULL;
    yang_stmt          *yspec = NULL;
    char               *yangfilename = NULL;
    char               *xmlfilename = NULL;
    char               *dbdir = NULL;
    int                 ret;
    uint32_t            id;
    enum operation_type op;
    cxobj              *xt = NULL;
    int                 i;
    char               *xpath;
    cbuf               *cbret = NULL;
    int                 dbg = 0;
    cxobj              *xerr = NULL;
    cxobj              *xcfg = NULL;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__FILE__, LOG_INFO, CLICON_LOG_STDERR); 

    argv0 = argv[0];
    /* Defaults */
    if ((h = clicon_handle_init()) == NULL)
        goto done;
    if ((xcfg = xml_new("clixon-config", NULL, CX_ELMNT)) == NULL)
        goto done;
    if (clicon_conf_xml_set(h, xcfg) < 0)
        goto done;
    /* getopt in two steps, first find config-file before over-riding options. */
    clicon_option_str_set(h, "CLICON_XMLDB_FORMAT", "xml"); /* default */
    while ((c = getopt(argc, argv, DATASTORE_OPTS)) != -1)
        switch (c) {
        case '?' :
        case 'h' : /* help */
            usage(argv0);
            break;
        case 'D' : /* debug */
            dbg = 1;    
            break;
        case 'd': /* db symbolic: running|candidate|startup */
            if (!optarg)
                usage(argv0);
            db = optarg;
            break;
        case 'b': /* db directory */
            if (!optarg)
                usage(argv0);
            dbdir = optarg;
            break;
        case 'f': /* db format */
            if (!optarg)
                usage(argv0);
            clicon_option_str_set(h, "CLICON_XMLDB_FORMAT", optarg);
            break;
        case 'x': /* XML file */
            if (!optarg)
                usage(argv0);
            xmlfilename = optarg;
            break;
        case 'y': /* Yang file */
            if (!optarg)
                usage(argv0);
            yangfilename = optarg;
            break;
        case 'Y':
            if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
                goto done;
            break;
        }
    /* 
     * Logs, error and debug to stderr, set debug level
     */
    clicon_log_init(__FILE__, dbg?LOG_DEBUG:LOG_INFO, CLICON_LOG_STDERR); 
    clicon_debug_init(dbg, NULL); 

    argc -= optind;
    argv += optind;
    if (argc < 1)
        usage(argv0);
    cmd = argv[0];
    if (dbdir == NULL){
        clicon_err(OE_DB, 0, "Missing dbdir -b option");
        goto done;
    }
    if (yangfilename == NULL){
        clicon_err(OE_YANG, 0, "Missing yang filename -y option");
        goto done;
    }
    /* Connect to plugin to get a handle */
    if (xmldb_connect(h) < 0)
        goto done;
    /* Create yang spec */
    if ((yspec = yspec_new()) == NULL)
        goto done;
    /* Parse yang spec from given file */
    if (yang_spec_parse_file(h, yangfilename, yspec) < 0)
        goto done;
    clicon_option_str_set(h, "CLICON_XMLDB_DIR", dbdir);
    clicon_dbspec_yang_set(h, yspec);
    if (strcmp(cmd, "get")==0){
        if (argc != 1 && argc != 2)
            usage(argv0);
        if (argc==2)
            xpath = argv[1];
        else
            xpath = "/";
        if (xmldb_get(h, db, NULL, xpath, &xt) < 0)
            goto done;
        if (clixon_xml2file(stdout, xt, 0, 0, fprintf, 0, 0) < 0)
            goto done;
        fprintf(stdout, "\n");
        if (xt){
            xml_free(xt);
            xt = NULL;
        }
    }
    else if (strcmp(cmd, "mget")==0){
        int nr;
        if (argc != 2 && argc != 3)
            usage(argv0);
        nr = atoi(argv[1]);
        if (argc==3)
            xpath = argv[2];
        else
            xpath = "/";
        for (i=0;i<nr;i++){
            if (xmldb_get(h, db, NULL, xpath, &xt) < 0)
                goto done;
            if (xt == NULL){
                clicon_err(OE_DB, 0, "xt is NULL");
                goto done;
            }
            if (clixon_xml2file(stdout, xt, 0, 0, fprintf, 0, 0) < 0)
                goto done;
            if (xt){
                xml_free(xt);
                xt = NULL;
            }
        }
        fprintf(stdout, "\n");
    }
    else if (strcmp(cmd, "put")==0){
        if (argc == 2){
            if (xmlfilename == NULL){
                clicon_err(OE_DB, 0, "XML filename expected");
                usage(argv0);
            }
        }
        else if (argc != 3){
            clicon_err(OE_DB, 0, "Unexpected nr of args: %d", argc);
            usage(argv0);
        }
        if (xml_operation(argv[1], &op) < 0){
            clicon_err(OE_DB, 0, "Unrecognized operation: %s", argv[1]);
            usage(argv0);
        }
        if (argc == 2){
            FILE *fp;
            if ((fp = fopen(xmlfilename, "r")) == NULL){
                clicon_err(OE_UNIX, errno, "fopen(%s)", xmlfilename);
                goto done;
            }
            if (clixon_xml_parse_file(fp, YB_MODULE, yspec, &xt, NULL) < 0)
                goto done;
            fclose(fp);
        }
        else{
            if ((ret = clixon_xml_parse_string(argv[2], YB_MODULE, yspec, &xt, &xerr)) < 0)
                goto done;
            if (ret == 0){
                xml_print(stderr, xerr);
                goto done;
            }
        }
        if (xml_name_set(xt, NETCONF_INPUT_CONFIG) < 0)
            goto done;
        if ((cbret = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        if ((ret = xmldb_put(h, db, op, xt, NULL, cbret)) < 0)
            goto done;
    }
    else if (strcmp(cmd, "copy")==0){
        if (argc != 2)
            usage(argv0);
        if (xmldb_copy(h, db, argv[1]) < 0)
            goto done;
    }
    else if (strcmp(cmd, "lock")==0){
        if (argc != 2)
            usage(argv0);
        id = atoi(argv[1]);
        if (xmldb_lock(h, db, id) < 0)
            goto done;
    }
    else if (strcmp(cmd, "unlock")==0){
        if (argc != 1)
            usage(argv0);
        if (xmldb_unlock(h, db) < 0)
            goto done;
    }
    else if (strcmp(cmd, "unlock_all")==0){
        if (argc != 2)
            usage(argv0);
        id = atoi(argv[1]);
        if (xmldb_unlock_all(h, id) < 0)
            goto done;
    }
    else if (strcmp(cmd, "islocked")==0){
        if (argc != 1)
            usage(argv0);
        if ((ret = xmldb_islocked(h, db)) < 0)
            goto done;
        fprintf(stdout, "islocked: %d\n", ret);
    }
    else if (strcmp(cmd, "exists")==0){
        if (argc != 1)
            usage(argv0);
        if ((ret = xmldb_exists(h, db)) < 0)
            goto done;
        fprintf(stdout, "exists: %d\n", ret);
    }
    else if (strcmp(cmd, "delete")==0){
        if (argc != 1)
            usage(argv0);
        if (xmldb_delete(h, db) < 0)
            goto done;
    }
    else if (strcmp(cmd, "init")==0){
        if (argc != 1)
            usage(argv0);
        if (xmldb_create(h, db) < 0)
            goto done;
    }
    else{
        clicon_err(OE_DB, 0, "Unrecognized command: %s", cmd);
        usage(argv0);
    }
    if (xmldb_disconnect(h) < 0)
        goto done;
    retval = 0;
  done:
    if (xcfg)
        xml_free(xcfg);
    if (cbret)
        cbuf_free(cbret);
    if (xt)
        xml_free(xt);
    if (h)
        clicon_handle_exit(h);
    if (yspec)
        ys_free(yspec);
    return retval;
}

