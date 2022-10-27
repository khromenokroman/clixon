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

 *
 * Protocol to communicate between clients (eg clixon_cli, clixon_netconf) 
 * and server (clicon_backend)
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_event.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_sig.h"
#include "clixon_xml.h"
#include "clixon_xml_io.h"
#include "clixon_netconf_lib.h"
#include "clixon_options.h"
#include "clixon_proto.h"

static int _atomicio_sig = 0;

/*! Formats (showas) derived from XML
 */
struct formatvec{
    char *fv_str;
    int   fv_int;
};

static struct formatvec _FORMATS[] = {
    {"xml",     FORMAT_XML},
    {"text",    FORMAT_TEXT},
    {"json",    FORMAT_JSON},
    {"cli",     FORMAT_CLI},
    {"netconf", FORMAT_NETCONF},
    {NULL,   -1}
};

/*! Translate from numeric format to string representation
 * @param[in]  showas   Format value (see enum format_enum)
 * @retval     str      String value
 */
char *
format_int2str(enum format_enum showas)
{
    struct formatvec *fv;

    for (fv=_FORMATS; fv->fv_int != -1; fv++)
        if (fv->fv_int == showas)
            break;
    return fv?(fv->fv_str?fv->fv_str:"unknown"):"unknown";
}

/*! Translate from string to numeric format representation
 * @param[in]  str       String value
 * @retval     enum      Format value (see enum format_enum)
 */
enum format_enum
format_str2int(char *str)
{
    struct formatvec *fv;

    for (fv=_FORMATS; fv->fv_int != -1; fv++)
        if (strcmp(fv->fv_str, str) == 0)
            break;
    return fv?fv->fv_int:-1;
}

/*! Encode a clicon netconf message using variable argument lists
 * @param[in] id      Session id of client
 * @param[in] format  Variable agrument list format an XML netconf string
 * @retval    NULL    Error
 * @retval    msg     Clicon message to send to eg clicon_msg_send()
 * @note if format includes %, they will be expanded according to printf rules.
 *       if this is a problem, use ("%s", xml) instaead of (xml)
 *       Notaly this may an issue of RFC 3896 encoded strings
 */
struct clicon_msg *
clicon_msg_encode(uint32_t      id,
                  const char   *format, ...)
{
    va_list            args;
    uint32_t           xmllen;
    uint32_t           len;
    struct clicon_msg *msg = NULL;
    int                hdrlen = sizeof(*msg);

    va_start(args, format);
    xmllen = vsnprintf(NULL, 0, format, args) + 1;
    va_end(args);

    len = hdrlen + xmllen;
    if ((msg = (struct clicon_msg *)malloc(len)) == NULL){
        clicon_err(OE_PROTO, errno, "malloc");
        return NULL;
    }
    memset(msg, 0, len);
    /* hdr */
    msg->op_len = htonl(len);
    msg->op_id = htonl(id);
    
    /* body */
    va_start(args, format);
    vsnprintf(msg->op_body, xmllen, format, args);
    va_end(args);

    return msg;
}

/*! Decode a clicon netconf message
 * @param[in]  msg    CLICON msg
 * @param[in]  yspec  Yang specification, (can be NULL)
 * @param[out] id     Session id
 * @param[out] xml    XML parse tree
 * @param[out] xerr   Reason for failure (yang assignment not made) if retval =0
 * @retval     1      Parse OK and all yang assignment made
 * @retval     0      Parse OK but yang assigment not made (or only partial)
 * @retval    -1      Error with clicon_err called. Includes parse error
 */
int
clicon_msg_decode(struct clicon_msg *msg, 
                  yang_stmt         *yspec,
                  uint32_t          *id,
                  cxobj            **xml,
                  cxobj            **xerr)
{
    int    retval = -1;
    char  *xmlstr;
    int    ret;

    /* hdr */
    if (id)
        *id = ntohl(msg->op_id);
    /* body */
    xmlstr = msg->op_body;
    clicon_debug(1, "%s %s", __FUNCTION__, xmlstr);
    if ((ret = clixon_xml_parse_string(xmlstr, yspec?YB_RPC:YB_NONE, yspec, xml, xerr)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Open local connection using unix domain sockets
 * @param[in]  h        Clicon handle
 * @param[in]  sockpath Unix domain file path
 * @retval     s        socket
 * @retval     -1       error
 */
int
clicon_connect_unix(clicon_handle h,
                    char         *sockpath)
{
    struct sockaddr_un addr;
    int retval = -1;
    int s;

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        clicon_err(OE_CFG, errno, "socket");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path)-1);

    clicon_debug(2, "%s: connecting to %s", __FUNCTION__, addr.sun_path);
    if (connect(s, (struct sockaddr *)&addr, SUN_LEN(&addr)) < 0){
        if (errno == EACCES)
            clicon_err(OE_CFG, errno, "connecting unix socket: %s. "
                       "Is user not member of group: \"%s\"?", 
                       sockpath, clicon_sock_group(h));
        else
            clicon_err(OE_CFG, errno, "connecting unix socket: %s", sockpath);
        close(s);
        goto done;
    }
    retval = s;
  done:
    return retval;
}

static void
atomicio_sig_handler(int arg)
{
    _atomicio_sig++;
}

/*! Ensure all of data on socket comes through. fn is either read or write
 * @param[in]  fn  I/O function, ie read/write
 * @param[in]  fd  File descriptor, eg socket
 * @param[in]  s0  Buffer to read to or write from
 * @param[in]  n   Number of bytes to read/write, loop until done
 */
static ssize_t
atomicio(ssize_t (*fn) (int, void *, size_t), 
         int       fd, 
         void     *s0, 
         size_t    n)
{
    char *s = s0;
    ssize_t res, pos = 0;

    while (n > pos) {
        _atomicio_sig = 0;
        res = (fn)(fd, s + pos, n - pos);
        switch (res) {
        case -1:
            if (errno == EINTR){
                if (!_atomicio_sig)
                    continue;
            }
            else if (errno == EAGAIN)
                continue;
            else if (errno == ECONNRESET)/* Connection reset by peer */
                res = 0;
            else if (errno == EPIPE)     /* Client shutdown */
                res = 0;
            else if (errno == EBADF)     /* client shutdown - freebsd */
                res = 0;
        case 0: /* fall thru */
            return (res);
        default:
            pos += res;
        }
    }
    return (pos);
}

/*! Print message on debug. Log if syslog, stderr if not
 * @param[in]  msg    CLICON msg
 */
static int
msg_dump(struct clicon_msg *msg)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   i;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_CFG, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s:", __FUNCTION__);
    for (i=0; i<ntohl(msg->op_len); i++){
        cprintf(cb, "%02x", ((char*)msg)[i]&0xff);
        if ((i+1)%32==0){
            clicon_debug(2, "%s", cbuf_get(cb));
            cbuf_reset(cb);
            cprintf(cb, "%s:", __FUNCTION__);
        }
        else
            if ((i+1)%4==0)
                cprintf(cb, " ");
    }
    clicon_debug(2, "%s", cbuf_get(cb));
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send a CLICON netconf message using internal IPC message
 *
 * @param[in]   s      socket (unix or inet) to communicate with backend
 * @param[out]  msg    CLICON msg data reply structure. Free with free()
 * @see clicon_msg_send1  using plain NETCONF
 */
int
clicon_msg_send(int                s, 
                struct clicon_msg *msg)
{ 
    int retval = -1;
    int e;

    clicon_debug(2, "%s: send msg len=%d", 
                 __FUNCTION__, ntohl(msg->op_len));
    if (clicon_debug_get() > 2)
        msg_dump(msg);
    if (atomicio((ssize_t (*)(int, void *, size_t))write, 
                 s, msg, ntohl(msg->op_len)) < 0){
        e = errno;
        clicon_err(OE_CFG, e, "atomicio");
        clicon_log(LOG_WARNING, "%s: write: %s len:%u msg:%s", __FUNCTION__,
                   strerror(e), ntohs(msg->op_len), msg->op_body);
        goto done;
    }
    retval = 0;
  done:
    return retval;
}

/*! Receive a CLICON message using IPC message struct
 *
 * XXX: timeout? and signals?
 * There is rudimentary code for turning on signals and handling them 
 * so that they can be interrupted by ^C. But the problem is that this
 * is a library routine and such things should be set up in the cli 
 * application for example: a daemon calling this function will want another 
 * behaviour.
 * Now, ^C will interrupt the whole process, and this may not be what you want.
 *
 * @param[in]   s      socket (unix or inet) to communicate with backend
 * @param[out]  msg    CLICON msg data reply structure. Free with free()
 * @param[out]  eof    Set if eof encountered
 * Note: caller must ensure that s is closed if eof is set after call.
 * @see clicon_msg_rcv1 using plain NETCONF
 */
int
clicon_msg_rcv(int                s,
               struct clicon_msg **msg,
               int                *eof)
{ 
    int       retval = -1;
    struct clicon_msg hdr;
    int       hlen;
    ssize_t   len2;
    sigfn_t   oldhandler;
    uint32_t  mlen;

    *eof = 0;
    if (0)
        set_signal(SIGINT, atomicio_sig_handler, &oldhandler);

    if ((hlen = atomicio(read, s, &hdr, sizeof(hdr))) < 0){ 
        clicon_err(OE_CFG, errno, "atomicio");
        goto done;
    }
    if (hlen == 0){
        retval = 0;
        *eof = 1;
        goto done;
    }
    if (hlen != sizeof(hdr)){
        clicon_err(OE_CFG, errno, "header too short (%d)", hlen);
        goto done;
    }
    mlen = ntohl(hdr.op_len);
    clicon_debug(2, "%s: rcv msg len=%d",  
                 __FUNCTION__, mlen);
    if ((*msg = (struct clicon_msg *)malloc(mlen)) == NULL){
        clicon_err(OE_CFG, errno, "malloc");
        goto done;
    }
    memcpy(*msg, &hdr, hlen);
    if ((len2 = atomicio(read, s, (*msg)->op_body, mlen - sizeof(hdr))) < 0){ 
        clicon_err(OE_CFG, errno, "read");
        goto done;
    }
    if (len2 != mlen - sizeof(hdr)){
        clicon_err(OE_CFG, errno, "body too short");
        goto done;
    }
    if (clicon_debug_get() > 1)
        msg_dump(*msg);
    retval = 0;
  done:
    if (0)
        set_signal(SIGINT, oldhandler, NULL);
    return retval;
}

/*! Receive a message using plain NETCONF
 *
 * @param[in]   s      socket (unix or inet) to communicate with backend
 * @param[out]  cb     cligen buf struct containing the incoming message
 * @param[out]  eof    Set if eof encountered
 * @see netconf_input_cb()
 * @see clicon_msg_rcv using IPC message struct
 */
int
clicon_msg_rcv1(int   s,
                cbuf *cb,
                int  *eof)
{
    int           retval = -1;
    unsigned char buf[BUFSIZ];
    int           i;
    int           len;
    int           xml_state = 0;
    int           poll;

    clicon_debug(1, "%s", __FUNCTION__);
    *eof = 0;
    memset(buf, 0, sizeof(buf));
    while (1){
       if ((len = read(s, buf, sizeof(buf))) < 0){
           if (errno == ECONNRESET)
               len = 0; /* emulate EOF */
           else{
               clicon_log(LOG_ERR, "%s: read: %s errno:%d", __FUNCTION__, strerror(errno), errno);
               goto done;
           }
       } /* read */
       if (len == 0){  /* EOF */
           *eof = 1;
           close(s);
           goto ok;
       }
       for (i=0; i<len; i++){
           if (buf[i] == 0)
               continue; /* Skip NULL chars (eg from terminals) */
           cprintf(cb, "%c", buf[i]);
           if (detect_endtag("]]>]]>",
                             buf[i],
                             &xml_state)) {
               /* OK, we have an xml string from a client */
               /* Remove trailer */
               *(((char*)cbuf_get(cb)) + cbuf_len(cb) - strlen("]]>]]>")) = '\0';
               goto ok;
           }
       }
       /* poll==1 if more, poll==0 if none */
       if ((poll = clixon_event_poll(s)) < 0)
           goto done;
       if (poll == 0)
           break; /* No data to read */
    } /* while */
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s done", __FUNCTION__);
    return retval;
}

/*! Send a CLICON netconf message plain NETCONF
 *
 * @param[in]   s      socket (unix or inet) to communicate with backend
 * @param[out]  msg    CLICON msg data reply structure. Free with free()
 * @see clicon_msg_send  using internal IPC header
 */
int
clicon_msg_send1(int   s, 
                 cbuf *cb)
{ 
    int retval = -1;

    if (atomicio((ssize_t (*)(int, void *, size_t))write, 
                 s, cbuf_get(cb), cbuf_len(cb)+1) < 0){
        clicon_err(OE_CFG, errno, "atomicio");
        clicon_log(LOG_WARNING, "%s: write: %s", __FUNCTION__, strerror(errno));
        goto done;
    }
    retval = 0;
  done:
    return retval;
}

/*! Connect to server, send a clicon_msg message and wait for result using unix socket
 *
 * @param[in]  h       Clicon handle
 * @param[in]  msg     CLICON msg data structure. It has fixed header and variable body.
 * @param[in]  sockpath Unix domain file path
 * @param[out] retdata  Returned data as string netconf xml tree.
 * @param[out] sock0   Return socket in case of asynchronous notify
 * @retval     0       OK
 * @retval     -1      Error
 * @see clicon_rpc  But this is one-shot rpc: open, send, get reply and close.
 */
int
clicon_rpc_connect_unix(clicon_handle      h,
                        char              *sockpath,
                        int               *sock0)
{
    int         retval = -1;
    int         s = -1;
    struct stat sb = {0,};

    clicon_debug(1, "Send msg on %s", sockpath);
    if (sock0 == NULL){
        clicon_err(OE_NETCONF, EINVAL, "sock0 expected");
        goto done;
    }
    /* special error handling to get understandable messages (otherwise ENOENT) */
    if (stat(sockpath, &sb) < 0){
        clicon_err(OE_PROTO, errno, "%s: config daemon not running?", sockpath);
        goto done;
    }
    if (!S_ISSOCK(sb.st_mode)){
        clicon_err(OE_PROTO, EIO, "%s: Not unix socket", sockpath);
        goto done;
    }
    if ((s = clicon_connect_unix(h, sockpath)) < 0)
        goto done;
    *sock0 = s;
    retval = 0;
  done:
    return retval;
}

/*! Connect to server, send a clicon_msg message and wait for result using an inet socket
 *
 * @param[in]  h       Clicon handle (not used)
 * @param[in]  dst     IPv4 address
 * @param[in]  port    TCP port
 * @param[out] retdata  Returned data as string netconf xml tree.
 * @param[out] sock0   Return socket in case of asynchronous notify
 * @retval     0       OK
 * @retval     -1      Error
 * @see clicon_rpc  But this is one-shot rpc: open, send, get reply and close.
 */
int
clicon_rpc_connect_inet(clicon_handle      h,
                        char              *dst,
                        uint16_t           port,
                        int               *sock0)
{
    int                retval = -1;
    int                s = -1;
    struct sockaddr_in addr;

    clicon_debug(1, "Send msg to %s:%hu", dst, port);
    if (sock0 == NULL){
        clicon_err(OE_NETCONF, EINVAL, "sock0 expected");
        goto done;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(addr.sin_family, dst, &addr.sin_addr) != 1)
        goto done; /* Could check getaddrinfo */
    
    /* special error handling to get understandable messages (otherwise ENOENT) */
    if ((s = socket(addr.sin_family, SOCK_STREAM, 0)) < 0) {
        clicon_err(OE_CFG, errno, "socket");
        return -1;
    }
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        clicon_err(OE_CFG, errno, "connecting socket inet4");
        close(s);
        goto done;
    }
    *sock0 = s;
    retval = 0;
  done:
    return retval;
}

/*! Send a clicon_msg message and wait for result.
 *
 * TBD: timeout, interrupt?
 * retval may be -1 and
 * errno set to ENOTCONN/ESHUTDOWN which means that socket is now closed probably
 * due to remote peer disconnecting. The caller may have to do something,...
 *
 * @param[in]  sock    Socket / file descriptor
 * @param[in]  msg     CLICON msg data structure. It has fixed header and variable body.
 * @param[out] xret    Returned data as netconf xml tree.
 * @param[out] eof     Set if eof encountered
 * @retval     0       OK (check eof)
 * @retval     -1      Error
 * @see clicon_rpc1 using plain NETCONF XML
 */
int
clicon_rpc(int                sock,
           struct clicon_msg *msg, 
           char             **ret,
           int               *eof)
{
    int                retval = -1;
    struct clicon_msg *reply = NULL;
    char              *data = NULL;

    if (clicon_msg_send(sock, msg) < 0)
        goto done;
    if (clicon_msg_rcv(sock, &reply, eof) < 0)
        goto done;
    if (*eof)
        goto ok;
    data = reply->op_body; /* assume string */
    if (ret && data)
        if ((*ret = strdup(data)) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
 ok:
    retval = 0;
  done:
    if (reply)
        free(reply);
    return retval;
}

/*! Send a netconf message and recieve result using plain NETCONF
 *
 * This is mainly used by the client API. 
 *
 * @param[in]  sock    Socket / file descriptor
 * @param[in]  msgin   CLICON msg data structure. It has fixed header and variable body.
 * @param[out] msgret  Returned data as netconf xml tree.
 * @param[out] eof     Set if eof encountered
 * @retval     0       OK
 * @retval     -1      Error
 * @see clicon_rpc using clicon_msg protocol header
 */
int
clicon_rpc1(int   sock,
            cbuf *msg,
            cbuf *msgret,
            int  *eof)
{
    int    retval = -1;

    clicon_debug(1, "%s", __FUNCTION__);
    if (netconf_framing_preamble(NETCONF_SSH_CHUNKED, msg) < 0)
        goto done;
    if (netconf_framing_postamble(NETCONF_SSH_CHUNKED, msg) < 0)
        goto done;
    if (clicon_msg_send1(sock, msg) < 0)
        goto done;
    if (clicon_msg_rcv1(sock, msgret, eof) < 0)
        goto done;
    retval = 0;
  done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

/*! Send a clicon_msg message as reply to a clicon rpc request
 *
 * @param[in]  s       Socket to communicate with client
 * @param[in]  data    Returned data as byte-string.
 * @param[in]  datalen Length of returned data XXX  may be unecessary if always string?
 * @retval     0       OK
 * @retval     -1      Error
 */
int 
send_msg_reply(int      s, 
               char    *data, 
               uint32_t datalen)
{
    int                retval = -1;
    struct clicon_msg *reply = NULL;
    uint32_t           len;

    len = sizeof(*reply) + datalen;
    if ((reply = (struct clicon_msg *)malloc(len)) == NULL)
        goto done;
    memset(reply, 0, len);
    reply->op_len = htonl(len);
    if (datalen > 0)
      memcpy(reply->op_body, data, datalen);
    if (clicon_msg_send(s, reply) < 0)
        goto done;
    retval = 0;
  done:
    if (reply)
        free(reply);
    return retval;
}

/*! Send a clicon_msg NOTIFY message asynchronously to client
 *
 * @param[in]  s       Socket to communicate with client
 * @param[in]  level
 * @param[in]  event
 * @retval     0       OK
 * @retval     -1      Error
 * @see send_msg_notify_xml
 */
static int
send_msg_notify(int           s, 
                char         *event)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;

    if ((msg=clicon_msg_encode(0, "%s", event)) == NULL)
        goto done;
    if (clicon_msg_send(s, msg) < 0)
        goto done;
    retval = 0;
  done:
    if (msg)
        free(msg);
    return retval;
}

/*! Send a clicon_msg NOTIFY message asynchronously to client
 *
 * @param[in]  h       Clicon handle
 * @param[in]  s       Socket to communicate with client
 * @param[in]  xev     Event as XML
 * @retval     0       OK
 * @retval     -1      Error
 * @see send_msg_notify XXX beauty contest
 */
int
send_msg_notify_xml(clicon_handle h,
                    int           s, 
                    cxobj        *xev)
{
    int                retval = -1;
    cbuf              *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (clixon_xml2cbuf(cb, xev, 0, 0, -1, 0) < 0)
        goto done;
    if (send_msg_notify(s, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
  done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Look for a text pattern in an input string, one char at a time
 * @param[in]     tag     What to look for
 * @param[in]     ch      New input character
 * @param[in,out] state   A state integer holding how far we have parsed.
 * @retval        0       No, we havent detected end tag
 * @retval        1       Yes, we have detected end tag!
 * @code
 *   int state = 0;
 *   char ch;
 *   while (1) {
 *     // read ch
 *     if (detect_endtag("mypattern", ch, &state)) {
 *       // mypattern is matched
 *     }
 *   }
 * @endcode
 */
int
detect_endtag(char *tag, 
              char  ch, 
              int  *state)
{
    int retval = 0;

    if (tag[*state] == ch){
        (*state)++;
        if (*state == strlen(tag)){
            *state = 0;
            retval = 1;
        }
    }
    else
        *state = 0;
    return retval;
}

/*! Given family, addr str, port, return sockaddr and length
 *
 * @param[in]  addrtype  Address family: inet:ipv4-address or inet:ipv6-address
 * @param[in]  addrstr   IP address as string
 * @param[in]  port      TCP port host byte order
 * @param[out] sa        sockaddr, should be allocated
 * @param[out] salen     length of sockaddr data
 * @code
 *    struct sockaddr_in6 sin6 = {0,}; // because its larger than sin and sa
 *    struct sockaddr    *sa = &sin6;
 *    size_t              sa_len;
 *    if (clixon_inet2sin(inet:ipv4-address, "0.0.0.0", 80, sa, &sa_len) < 0)
 *       err;
 * @endcode
 * Probably misplaced, need a clixon_network file?
 */
int
clixon_inet2sin(const char       *addrtype,
                const char       *addrstr,
                uint16_t          port,
                struct sockaddr  *sa,
                size_t           *sa_len)
{
    struct sockaddr_in6 *sin6;
    struct sockaddr_in  *sin;

    if (strcmp(addrtype, "inet:ipv6-address") == 0) {
        sin6 = (struct sockaddr_in6 *)sa;
        *sa_len          = sizeof(struct sockaddr_in6);
        sin6->sin6_port   = htons(port);
        sin6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, addrstr, &sin6->sin6_addr);
    }
    else if (strcmp(addrtype, "inet:ipv4-address") == 0) {
        sin = (struct sockaddr_in *)sa;
        *sa_len             = sizeof(struct sockaddr_in);
        sin->sin_family      = AF_INET;
        sin->sin_port        = htons(port);
        sin->sin_addr.s_addr = inet_addr(addrstr);
    }
    else{
        clicon_err(OE_XML, EINVAL, "Unexpected addrtype: %s", addrtype);
        return -1;
    }
    return 0;
}
