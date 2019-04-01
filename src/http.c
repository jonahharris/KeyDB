/*
 * Copyright (c) 2019 Jonah H. Harris <jonah.harris@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "picohttpparser.h"

void
addReplyHTTPHeader (
  client               *c,
  const char           *header,
  const char           *value
) {
  sds http_header = sdscatfmt(sdsempty(), "%s: %s\r\n",
    header, value);
  addReplyProto(c, http_header, sdslen(http_header));
  sdsfree(http_header);
}

void
processHTTPRequest (
  client               *c
) {
  const char *method;
  size_t method_len;
  const char *path;
  size_t path_len;
  int minor_version;
  struct phr_header headers[64];
  size_t num_headers;
  int pret;

  num_headers = sizeof(headers) / sizeof(headers[0]);
  pret = phr_parse_request(c->querybuf, sdslen(c->querybuf), &method, &method_len,
    &path, &path_len, &minor_version, headers, &num_headers, 0);
  switch (pret) {
    case -2:
      printf("Incomplete HTTP request... need more data");
      return;
      break;
    case -1:
      printf("HTTP parse error\n");
      return;
      break;
    case 0:
      printf("No data?\n");
      return;
      break;
    default:
      break;
  }

  printf("request is %d bytes long\n", pret);
  printf("method is %.*s\n", (int)method_len, method);
  printf("path is %.*s\n", (int)path_len, path);
  printf("HTTP version is 1.%d\n", minor_version);
  printf("headers:\n");
  for (size_t i = 0; i != num_headers; ++i) {
      printf("%.*s: %.*s\n", (int)headers[i].name_len, headers[i].name,
             (int)headers[i].value_len, headers[i].value);
  }

  /* FIXME crappy assumption of leading '/' */
  if (0 < path_len) {
    ++path;
    --path_len;
  }

  robj *keyname = 0 == path_len ? createStringObject("/",1) : createStringObject(path, path_len);
  robj *o = lookupKeyRead(c->db,keyname);

  /* If there is no such key, return with a HTTP error. */
  if (o == NULL || o->type != OBJ_STRING) {
      char *errstr;
      if (o == NULL)
          errstr = "Error: no content at the specified key";
      else
          errstr = "Error: selected key type is invalid "
                   "for HTTP output";
      addReplyProto(c, "HTTP/1.0 404 Not Found\r\n\r\n", 26);
  } else {
      addReplyProto(c, "HTTP/1.0 200 OK\r\n", 17);
      addReplyHTTPHeader(c, "Content-type", "text/plain");
      addReplyProto(c, "\r\n", 2);
      addReply(c,o);
  }

  if (0 == path_len) {
    decrRefCount(keyname);
  }
}
