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

 * Event notification streams according to RFC5277
 */
#ifndef _CLIXON_STREAM_H_
#define _CLIXON_STREAM_H_

/*
 * Types
 */
/* subscription callback */
typedef	int (*stream_fn_t)(clicon_handle, void *filter, void *arg);
typedef	stream_fn_t subscription_fn_t;

struct stream_subscription{
    struct stream_subscription *ss_next;
    char                       *ss_stream; /* Name of associated stream */
    stream_fn_t                 ss_fn;     /* Callback when event occurs */
    void                       *ss_arg;    /* Callback argument */
};

/* See RFC8040 9.3, stream list, no replay support for now
 */
struct event_stream{
    struct event_stream *es_next;
    char                *es_name; /* name of notification event stream */
    char                *es_description;  
    struct stream_subscription *es_subscription;
};
typedef struct event_stream event_stream_t;

/*
 * Prototypes
 */
event_stream_t *stream_find(clicon_handle h, const char *name);
int stream_register(clicon_handle h, const char *name, const char *description);
int stream_free(event_stream_t *es);
int stream_get_xml(clicon_handle h, int access, cbuf *cb);
int stream_cb_add(clicon_handle h, char *stream, stream_fn_t fn, void *arg);
int stream_cb_delete(clicon_handle h, char *stream, stream_fn_t fn);

#endif /* _CLIXON_STREAM_H_ */
