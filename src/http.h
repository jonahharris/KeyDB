#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct HttpClient
{
  int fd;
  size_t cbQuery;
  size_t cbQueryAlloc;
  char *szQuery;
};

void processHTTPRequest(aeEventLoop *el, struct HttpClient *pclient);

#ifdef __cplusplus
}
#endif