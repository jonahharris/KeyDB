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
#include "http.h"
#include "picohttpparser.h"

#define HTTP_READ_CHUNK 4096

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

void readHTTPRequest (aeEventLoop *el, int fd, void *privdata, int mask) {
  UNUSED(el);
  UNUSED(mask);
  struct HttpClient *pclient = (struct HttpClient*)privdata;
  /* BUGBUG This is just a proof of concept for the reading code, I'm sure there's a better way */
  if ((pclient->cbQueryAlloc - pclient->cbQuery) < HTTP_READ_CHUNK)
  {
    pclient->cbQueryAlloc += HTTP_READ_CHUNK; // REVIEW: Growth limits?
    pclient->szQuery = zrealloc(pclient->szQuery, pclient->cbQueryAlloc, MALLOC_LOCAL); // REVIEW handle NULL
  }
  ssize_t cbRead = read(fd, pclient->szQuery + pclient->cbQuery, HTTP_READ_CHUNK-1);
  if (cbRead < 0)
  {
    if (errno != EAGAIN)
      perror("Failed to read from HTTP socket");
    serverAssert(errno == EAGAIN);  // REVIEW: Handle other errors
    return;
  }
  else if (cbRead > 0)
  {
    pclient->cbQuery += cbRead;
    pclient->szQuery[pclient->cbQuery] = '\0';
    processHTTPRequest(el, pclient);
  }
}

#define MAX_ACCEPTS_PER_CALL 1000
void acceptHttpTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
  int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
  char cip[NET_IP_STR_LEN];
  UNUSED(mask);
  UNUSED(privdata);

  while(max--) 
  {
    cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
    if (cfd == ANET_ERR) {
        if (errno != EWOULDBLOCK)
            serverLog(LL_WARNING,
                "Accepting HTTP connection: %s", server.neterr);
        return;
    }
    serverLog(LL_VERBOSE,"HTTP Accepted %s:%d", cip, cport);

    struct HttpClient *pclient = (struct HttpClient*)zmalloc(sizeof(struct HttpClient), MALLOC_LOCAL);
    if (pclient != NULL) {
      pclient->fd = cfd;
      pclient->cbQuery = 0;
      pclient->cbQueryAlloc = 0;
      pclient->szQuery = NULL;
    }

    if (pclient == NULL || aeCreateFileEvent(el,pclient->fd,AE_READABLE, readHTTPRequest, pclient) == AE_ERR)
    {
      close(cfd);
      serverLog(LL_WARNING, "Failed to create HTTP listener event");
      return;
    }
  }
}

void processHTTPRequest(aeEventLoop *el, struct HttpClient *pclient)
{
  const char *method;
  size_t method_len;
  const char *path;
  size_t path_len;
  int minor_version;
  struct phr_header headers[64];
  size_t num_headers;
  int pret;
  int fd = pclient->fd;

  num_headers = sizeof(headers) / sizeof(headers[0]);
  pret = phr_parse_request(pclient->szQuery, pclient->cbQuery, &method, &method_len,
    &path, &path_len, &minor_version, headers, &num_headers, 0);
  switch (pret) {
    case -2:
      printf("Incomplete HTTP request... need more data\n");
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
  robj *o = lookupKeyRead(server.db,keyname); // REVIEW: We probably want to have more control over DB selection

  /* If there is no such key, return with a HTTP error. */
  if (o == NULL || o->type != OBJ_STRING) {
      char *errstr;
      if (o == NULL)
          errstr = "Error: no content at the specified key";
      else
          errstr = "Error: selected key type is invalid "
                   "for HTTP output";
      write(fd, "HTTP/1.0 404 Not Found\r\n\r\n", 26);
  } else {
    write(fd, "HTTP/1.0 200 OK\r\n", 17);
    write(fd, "Content-type: text/plain\r\n\r\n", 28);
    
    /* Note: This is stolen from addReply - it needs to be factored out into a common function */
    if (sdsEncodedObject(o)) {
        write(fd, (const char*)ptrFromObj(o), sdslen((sds)ptrFromObj(o)));
    } else if (o->encoding == OBJ_ENCODING_INT) {
        /* For integer encoded strings we just convert it into a string
         * using our optimized function, and attach the resulting string
         * to the output buffer. */
        char buf[32];
        size_t len = ll2string(buf,sizeof(buf),(long)ptrFromObj(o));
        write(fd, buf, len);
    } else {
        serverPanic("Wrong obj->encoding in addReply()");
    }
    write(fd, "\r\n", 2);
  }

  if (0 == path_len) {
    decrRefCount(keyname);
  }

  aeDeleteFileEvent(el, fd, AE_READABLE);
  close(fd);
  zfree(pclient);
}
