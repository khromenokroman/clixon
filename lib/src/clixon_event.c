/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgat)e

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
 * Event handling and loop
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <syslog.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>

#include <cligen/cligen.h>

#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_err.h"
#include "clixon_sig.h"
#include "clixon_proc.h"
#include "clixon_options.h"
#include "clixon_event_select.h"
#include "clixon_event.h"

/*
 * Constants
 */
#define EVENT_STRLEN 32

/*
 * Types
 */
struct event_data{
    struct event_data          *e_next;                 /* Next in list */
    int                       (*e_fn)(int, void*);      /* Callback function */
    enum {EVENT_FD, EVENT_TIME} e_type;                 /* Type of event */
    int                         e_fd;                   /* File descriptor */
    struct timeval              e_time;                 /* Timeout */
    void                       *e_arg;                  /* Function argument */
    char                        e_descr[EVENT_STRLEN]; /* String for debugging */
    struct pollfd              *e_pollfd;               /* Pointer to pull struct */
};

/*
 * Internal variables
 * Consider use handle variables instead of global, but needs API changes
 */

/* Cache event handling type since most event calls do not have handle parameter
 * @see  CLICON_EVENT_SELECT in clixon-config.yang
 * 0: poll, 1: select
 */
static int _event_select = 0;

/* File event handlers */
static struct event_data *_ee = NULL;
static int _ee_nr = 0;

/* Prioritized File event handlers */
static struct event_data *_ee_prio = NULL;
static int _ee_prio_nr = 0;

/* Timer event handlers */
static struct event_data *_ee_timers = NULL;

/* Set if element in _ee is deleted (clixon_event_unreg_fd). Check in _ee loops
 * XXX: algorithm has flaw: which _ee is unregged?
 */
static int _ee_unreg = 0;

/* If set (eg by signal handler) exit select loop on next run and return 0 */
static int _clicon_exit = 0;

/* If set (eg by signal handler) call waitpid on waiting processes, ignore EINTR, continue select loop */
static int _clicon_sig_child = 0;

/* If set (eg by signal handler) ignore EINTR and continue select loop */
static int _clicon_sig_ignore = 0;

/*! For signal handlers: instead of doing exit, set a global variable to exit
 *
 * - zero means dont exit,
 * - one means exit,
 * - more than one means decrement and make another event loop
 * Status is checked in event_loop and decremented by one.
 * When it reaches one the exit is made.
 * Note it maybe would be better to do use on a handle basis, but a signal
 * handler is global
 */
int
clixon_exit_set(int nr)
{
    _clicon_exit = nr;
    return 0;
}

/*! Get the status of global exit variable, usually set by signal handlers
 */
int
clixon_exit_get(void)
{
    return _clicon_exit;
}

/*! If > 1 decrement exit counter
 */
int
clixon_exit_decr(void)
{
    if (_clicon_exit > 1)
        _clicon_exit--;
    return 0;
}

int
clicon_sig_child_set(int val)
{
    _clicon_sig_child = val;
    return 0;
}

int
clicon_sig_child_get(void)
{
    return _clicon_sig_child;
}

int
clicon_sig_ignore_set(int val)
{
    _clicon_sig_ignore = val;
    return 0;
}

int
clicon_sig_ignore_get(void)
{
    return _clicon_sig_ignore;
}

/*! Register a callback function to be called on input on a file descriptor.
 *
 * Prio is primitive, non-preemptive as follows:
 * If several file events are active, then the prioritized are served first.
 * If a non-prioritized is running, and a prioritized becomes active, then the
 * running un-prioritized handler will run to completion (not pre-empted) and then
 * the priorizited events will run.
 * A timeout will always run.
 * @param[in]  fd   File descriptor
 * @param[in]  fn   Function to call when input available on fd
 * @param[in]  arg  Argument to function fn
 * @param[in]  str  Describing string for logging
 * @param[in]  prio Priority (0 or 1)
 * @code
 *   static int fn(int fd, void *arg){}
 *   clixon_event_reg_fd(fd, fn, (void*)42, "call fn on input on fd", 0);
 * @endcode
 * @see clixon_event_loop
 */
int
clixon_event_reg_fd_prio(int   fd,
                         int (*fn)(int, void*),
                         void *arg,
                         char *str,
                         int   prio)
{
    struct event_data *e;

    if (_event_select){
        return clixon_event_select_reg_fd_prio(fd, fn, arg, str, prio);
    }
    if ((e = (struct event_data *)malloc(sizeof(struct event_data))) == NULL){
        clixon_err(OE_EVENTS, errno, "malloc");
        return -1;
    }
    memset(e, 0, sizeof(struct event_data));
    strncpy(e->e_descr, str, EVENT_STRLEN-1);
    e->e_fd = fd;
    e->e_fn = fn;
    e->e_arg = arg;
    e->e_type = EVENT_FD;
    if (prio){
        e->e_next = _ee_prio;
        _ee_prio = e;
        _ee_prio_nr++;
    }
    else {
        e->e_next = _ee;
        _ee = e;
        _ee_nr++;
    }
    clixon_debug(CLIXON_DBG_EVENT, "registering %s", e->e_descr);
    return 0;
}

/*! Register un-prioritized file event callback
 *
 * @see clixon_event_unreg_fd_prio
 */
int
clixon_event_reg_fd(int   fd,
                    int (*fn)(int, void*),
                    void *arg,
                    char *str)
{
    return clixon_event_reg_fd_prio(fd, fn, arg, str, 0);
}

/*! Deregister a file descriptor callback
 *
 * @param[in]  s   File descriptor
 * @param[in]  fn  Function to call when input available on fd
 * @retval     0   OK
 * @retval    -1   Error
 * Note: deregister when exactly function and socket match, not argument
 * Consider adding prio to argument
 * @see clixon_event_reg_fd
 * @see clixon_event_unreg_timeout
 */
int
clixon_event_unreg_fd(int   s,
                      int (*fn)(int, void*))
{
    struct event_data *e;
    int                found = 0;
    struct event_data **e_prev;

    if (_event_select){
        return clixon_event_select_unreg_fd(s, fn);
    }
    /* First try prioritized */
    e_prev = &_ee_prio;
    for (e = _ee_prio; e; e = e->e_next){
        if (fn == e->e_fn && s == e->e_fd) {
            found++;
            *e_prev = e->e_next;
            _ee_prio_nr--;
            _ee_unreg++;
            free(e);
            break;
        }
        e_prev = &e->e_next;
    }
    if (!found){
        e_prev = &_ee;
        for (e = _ee; e; e = e->e_next){
            if (fn == e->e_fn && s == e->e_fd) {
                found++;
                *e_prev = e->e_next;
                _ee_nr--;
                _ee_unreg++;
                free(e);
                break;
            }
            e_prev = &e->e_next;
        }
    }
    return found?0:-1;
}

/*! Call a callback function at an absolute time
 *
 * @param[in]  t   Absolute (not relative!) timestamp when callback is called
 * @param[in]  fn  Function to call at time t
 * @param[in]  arg Argument to function fn
 * @param[in]  str Describing string for logging
 * @retval     0   OK
 * @retval    -1   Error
 * @code
 * int fn(int d, void *arg){
 *   struct timeval t, t1;
 *   gettimeofday(&t, NULL);
 *   t1.tv_sec = 1; t1.tv_usec = 0;
 *   timeradd(&t, &t1, &t);
 *   clixon_event_reg_timeout(t, fn, NULL, "call every second");
 * }
 * @endcode
 *
 * @note  The timestamp is an absolute timestamp, not relative.
 * @note  The callback is not periodic, you need to make a new registration for each period, see example.
 * @note  The first argument to fn is a dummy, just to get the same signature as for file-descriptor callbacks.
 * @see clixon_event_reg_fd
 * @see clixon_event_unreg_timeout
 */
int
clixon_event_reg_timeout(struct timeval t,
                         int          (*fn)(int, void*),
                         void          *arg,
                         char          *str)
{
    int                 retval = -1;
    struct event_data  *e;
    struct event_data  *e1;
    struct event_data **e_prev;

    if (_event_select){
        return clixon_event_select_reg_timeout(t, fn, arg, str);
    }
    if (str == NULL || fn == NULL){
        clixon_err(OE_CFG, EINVAL, "str or fn is NULL");
        goto done;
    }
    if ((e = (struct event_data *)malloc(sizeof(struct event_data))) == NULL){
        clixon_err(OE_EVENTS, errno, "malloc");
        return -1;
    }
    memset(e, 0, sizeof(struct event_data));
    strncpy(e->e_descr, str, EVENT_STRLEN-1);
    e->e_fn = fn;
    e->e_arg = arg;
    e->e_type = EVENT_TIME;
    e->e_time = t;
    /* Sort into right place */
    e_prev = &_ee_timers;
    for (e1=_ee_timers; e1; e1=e1->e_next){
        if (timercmp(&e->e_time, &e1->e_time, <))
            break;
        e_prev = &e1->e_next;
    }
    e->e_next = e1;
    *e_prev = e;
    clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "%s", str);
    retval = 0;
 done:
    return retval;
}

/*! Deregister a timeout callback as previosly registered by clixon_event_reg_timeout()
 *
 * Note: deregister when exactly function and function arguments match, not time. So you
 * cannot have same function and argument callback on different timeouts. This is a little
 * different from clixon_event_unreg_fd.
 * @param[in]  fn   Function to call at time t
 * @param[in]  arg  Argument to function fn
 * @retval     0    OK, timeout unregistered
 * @retval    -1    OK, but timeout not found
 * @see clixon_event_reg_timeout
 * @see clixon_event_unreg_fd
 */
int
clixon_event_unreg_timeout(int (*fn)(int, void*),
                           void *arg)
{
    struct event_data  *e;
    int                 found = 0;
    struct event_data **e_prev;

    if (_event_select){
        return clixon_event_select_unreg_timeout(fn, arg);
    }
    e_prev = &_ee_timers;
    for (e = _ee_timers; e; e = e->e_next){
        if (fn == e->e_fn && arg == e->e_arg) {
            found++;
            *e_prev = e->e_next;
            free(e);
            break;
        }
        e_prev = &e->e_next;
    }
    return found?0:-1;
}

/*! Poll to see if there is any data available on this file descriptor.
 *
 * @param[in]  fd   File descriptor
 * @retval    >0    Nr of elements to read on fd
 * @retval     0    Nothing to read/empty fd
 * @retval    -1    Error
 */
int
clixon_event_poll(int fd)
{
    int 	  retval = -1;
    struct pollfd pfd = {0,};
    int           ret;

    if (_event_select){
        return clixon_event_select_poll(fd);
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    if ((ret = poll(&pfd, 1, 0)) < 0){
        clixon_err(OE_EVENTS, errno, "poll");
        goto done;
    }
    retval = ret;
 done:
    return retval;
}

/*! Handle signal interrupt
 *
 * Signals are in three classes:
 * (1) Signals that exit gracefully, the function returns 0
 *     Must be registered such as by set_signal() of SIGTERM,SIGINT, etc with a
 *     handler that calls clicon_exit_set().
 * (2) SIGCHILD Childs that exit(), go through clixon_proc list and cal waitpid
 *     New select loop is called
 * (2) Signals are ignored, and the select is rerun, ie handler calls
 *     clicon_sig_ignore_get.
 *     New select loop is called
 * (3) Other signals result in an error and return -1.
 * @retval     1    OK
 * @retval     0    Exit
 * @retval    -1    Error
 */
static int
event_handle_eintr(clixon_handle h)
{
    int retval = -1;

    clixon_debug(CLIXON_DBG_EVENT, "poll/select %s", strerror(errno));
    if (clixon_exit_get() == 1){
        clixon_err(OE_EVENTS, errno, "poll/select");
        goto exit;
    }
    else if (clicon_sig_child_get()){
        /* Go through processes and wait for child processes */
        if (clixon_process_waitpid(h) < 0)
            goto done;
        clicon_sig_child_set(0);
    }
    else if (clicon_sig_ignore_get()){
        clicon_sig_ignore_set(0);
    }
    else{
        clixon_err(OE_EVENTS, errno, "poll/select");
        goto done;
    }
    retval = 1;
 done:
    return retval;
 exit:
    retval = 0;
    goto done;
}

static int
event_handle_fds(struct event_data *ee,
                 int                prio)
{
    int                retval = -1;
    struct pollfd     *pfd;
    struct event_data *e = NULL;

    for (e = ee; e; e = e->e_next) {
        if (e->e_type != EVENT_FD)
            continue;
        clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "check s:%d prio:%d fd %s", e->e_fd, prio, e->e_descr);
        if ((pfd = e->e_pollfd) == NULL) /* Could be added after poll regitsration */
            continue;
        if (pfd->revents != 0) { /* returned events */
            if (pfd->revents & POLLIN || pfd->revents & POLLHUP) {
                clixon_debug(CLIXON_DBG_EVENT, "fd %s", e->e_descr);
                _ee_unreg = 0;
                if ((*e->e_fn)(e->e_fd, e->e_arg) < 0) {
                    clixon_debug(CLIXON_DBG_EVENT, "Error in: %s", e->e_descr);
                    goto done;
                }
                if (_ee_unreg){ /* and this socket,... */
                    _ee_unreg = 0;
                    break;
                }
                if (prio == 0 && _ee_prio_nr > 0) /* Prioritized exists, break unprio fairness */
                    break;
            }
            else if (pfd->revents & POLLNVAL) { /* fd not open */
                clixon_err(OE_EVENTS, 0, "poll: Invalid request: %s fd %d not open",
                           e->e_descr, pfd->fd);
                goto done;
            }
            else {
                clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL,
                             "%s %d revents:0x%x", e->e_descr, pfd->fd, pfd->revents);
                goto done;
            }
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Dispatch file descriptor events (and timeouts) by invoking callbacks.
 *
 * @param[in] h  Clixon handle
 * @retval    0  OK
 * @retval   -1  Error: eg select, callback, timer,
 * @note There is an issue with fairness between timeouts and events
 *       Currently a socket that is not read/emptied properly starve timeouts.
 *       One could try to poll the file descriptors after a timeout?
 * TODO: unreg, better prio algorithm
 */
int
clixon_event_loop(clixon_handle h)
{
    int                retval = -1;
    struct event_data *e = NULL;
    struct pollfd     *fds = NULL;
    struct pollfd     *pfd;
    uint32_t           nfds_max = 0;
    int                nfds = 0;
    struct timeval     t0;
    struct timeval     t;
    int64_t            tdiff;
    int                timeout;
    int                n;
    int                ret;

    if (_event_select){
        return clixon_event_select_loop(h);
    }
    while (clixon_exit_get() != 1) {
        nfds = _ee_prio_nr + _ee_nr;
        if (nfds > nfds_max){
            nfds_max = nfds;
            if ((fds = realloc(fds, nfds_max*sizeof(struct pollfd))) == NULL){
                clixon_err(OE_UNIX, errno, "realloc");
                goto done;
            }
        }
        nfds = 0;
        clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "register prio");
        for (e = _ee_prio; e; e = e->e_next) {
            if (e->e_type == EVENT_FD) {
                pfd = &fds[nfds];
                pfd->fd = e->e_fd;
                pfd->events = POLLIN; /* requested event */
                e->e_pollfd = pfd;
                clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "register fd prio %s nr:%d",
                             e->e_descr, nfds);
                nfds++;
            }
        }
        clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "register unprio");
        for (e = _ee; e; e = e->e_next) {
            if (e->e_type == EVENT_FD) {
                pfd = &fds[nfds];
                pfd->fd = e->e_fd;
                pfd->events = POLLIN; /* requested event */
                e->e_pollfd = pfd;
                clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "register fd %s nr:%d",
                             e->e_descr, nfds);
                nfds++;
            }
        }
        if (nfds != _ee_nr + _ee_prio_nr){
            clixon_err(OE_EVENTS, 0, "File descriptor mismatch");
            goto done;
        }
        timeout = -1;
        clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "timeout");
        if (_ee_timers != NULL) {
            gettimeofday(&t0, NULL);
            timersub(&_ee_timers->e_time, &t0, &t);
            tdiff = t.tv_sec * 1000 + t.tv_usec / 1000;
            if (tdiff < 0)
                timeout = 0;
            else
                timeout = (int)tdiff;
        }
        clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "poll timeout: %d", timeout);
        n = poll(fds, nfds, timeout);
        if (n == -1) {
            int e = errno;
            clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "n=-1 Error: %d", e);
            if (e == EINTR){
                if (clixon_exit_get() == 1){
                    clixon_err(OE_EVENTS, errno, "poll");
                    goto ok;
                }
                if ((ret = event_handle_eintr(h)) < 0)
                    goto done;
                if (ret == 0){ // exit
                    retval = 0;
                    goto done;
                }
                continue;
            }
            else
                clixon_err(OE_EVENTS, errno, "poll");
            goto done;
        }
        if (n == 0) { /* timeout */
            clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "n=0 Timeout");
            e = _ee_timers;
            _ee_timers = _ee_timers->e_next;
            clixon_debug(CLIXON_DBG_EVENT | CLIXON_DBG_DETAIL, "timeout: %s", e->e_descr);
            if ((*e->e_fn)(0, e->e_arg) < 0) {
                free(e);
                goto done;
            }
            free(e);
        }
        /* Prio files */
        if ((ret = event_handle_fds(_ee_prio, 1)) < 0)
            goto done;
        /* Unprio files */
        if ((ret = event_handle_fds(_ee, 0)) < 0)
            goto done;
        clixon_exit_decr(); /* If exit is set and > 1, decrement it (and exit when 1) */
  }
 ok:
    if (clixon_exit_get() == 1)
        retval = 0;
 done:
    clixon_debug(CLIXON_DBG_EVENT, "retval:%d", retval);
    if (fds)
        free(fds);
    return retval;
}

int
clixon_event_exit(void)
{
    struct event_data *e;
    struct event_data *e_next;

    if (_event_select){
        return clixon_event_select_exit();
    }
    e_next = _ee_prio;
    while ((e = e_next) != NULL){
        e_next = e->e_next;
        free(e);
    }
    _ee_prio = NULL;

    e_next = _ee;
    while ((e = e_next) != NULL){
        e_next = e->e_next;
        free(e);
    }
    _ee = NULL;

    e_next = _ee_timers;
    while ((e = e_next) != NULL){
        e_next = e->e_next;
        free(e);
    }
    _ee_timers = NULL;
    return 0;
}

/*! Init clixon event handling
 *
 * Set which event handler to use: original select or new poll
 */
int
clixon_event_init(clixon_handle h)
{
    _event_select = clicon_option_bool(h, "CLICON_EVENT_SELECT");
    return 0;
}
