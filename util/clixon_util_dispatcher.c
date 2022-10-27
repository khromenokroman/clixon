/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2021-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

* Utility for testing path dispatcher
* Everything is run by options and order is significant which makes it a little special.
* For example:
* clixon_util_dispatcher -r -c / : 
*      Register cb1 with default path "/" and arg NULL, call with path /
* clixon_util_dispatcher -i 2 -p /foo -a bar -r -c /bar -c /fie
*      Register cb2 with path "/foo" and arg bar, call with path /bar then /fie
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
#include <stdint.h>
#include <syslog.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"
#include "clixon/clixon_backend.h"

/* Command line options to be passed to getopt(3) */
#define DISPATCHER_OPTS "hD:a:i:p:rc:"

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options]\n"
            "where options are\n"
            "\t-h \t\tHelp\n"
            "\t-D <level> \t Debug - print dispatch tree\n"
            "\t-a <string>\t Argument to callback (default: NULL)\n"
            "\t-i <int>   \t Function index: 1..3 (default: 1)\n"
            "\t-p <path>  \t Registered path (default: /)\n"
            "\t-r         \t Register callback (based on -a/-i/-p setting)\n"
            "\t-c <path>  \t Call dispatcher with path\n",
            argv0
            );
    exit(0);
}

/*! Function to handle a path
 *
 * @param[in]  h        Generic handler
 * @param[in]  xpath    Registered XPath using canonical prefixes
 * @param[in]  userargs Per-call user arguments
 * @param[in]  arg      Per-path user argument
 *(
/ * Make a CB() macro to generate simple callbacks that just prints the path and arg
 */
#define CB(i) static int cb##i(void *h0, char *xpath, void *userargs, void *arg)    {  fprintf(stdout, "%s %s\n", __FUNCTION__, (char*)arg); return 0; }

CB(1)
CB(2)

int
main(int    argc,
     char **argv)
{
    int                 retval = -1;
    char               *argv0 = argv[0];
    int                 logdst = CLICON_LOG_STDERR;
    int                 dbg = 0;
    int                 c;
    char               *arg = NULL;
    handler_function    fn = cb1;
    dispatcher_entry_t *htable = NULL;
    int                 ret;
    char               *regpath = "/"; /* Register path */
    
    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init("dispatcher", LOG_DEBUG, logdst); 
    
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, DISPATCHER_OPTS)) != -1)
        switch (c) {
        case 'h':
            usage(argv0);
            break;
        case 'D':
            if (sscanf(optarg, "%d", &dbg) != 1)
                usage(argv0);
            break;

        case 'a' :
        case 'i' :
        case 'p' :
        case 'r' :
        case 'c' :
            break;
        default:
            usage(argv[0]);
            break;
        }
    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init("xpath", dbg?LOG_DEBUG:LOG_INFO, logdst);
    
    clicon_debug_init(dbg, NULL);
            
    /* Now rest of options */   
    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv, DISPATCHER_OPTS)) != -1){
        switch (c) {
        case 'D' : /* debug */
            break; /* see above */
        case 'a' : /* arg string */
            arg = optarg;
            break;
        case 'i' : /* dispatcher function: 1..3 */
            switch (atoi(optarg)){
            case 1: fn = cb1; break;
            case 2: fn = cb2; break;
                //          case 3: fn = cb3; break;
            }
            break;
        case 'p' : /* register path */
            regpath = optarg;
            break;
        case 'r' :{ /* register callback based on -a/-i/-p*/
            dispatcher_definition     x = {regpath, fn, arg};
            if (dispatcher_register_handler(&htable, &x) < 0)
                goto done;          
            break;
        }
        case 'c':{ /* Execute a call using path */
            char *path = optarg;
            if ((ret = dispatcher_call_handlers(htable, NULL, path, NULL)) < 0)
                goto done;
            fprintf(stderr, "path:%s ret:%d\n", path, ret);
            break;
        }
        default:
            usage(argv[0]);
            break;
        }
    }
    if (dbg)
        dispatcher_print(stderr, 0, htable);
    dispatcher_free(htable);
    retval = 0;
 done:
    return retval;
}
