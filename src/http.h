#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void acceptHttpTcpHandler (aeEventLoop *el, int fd, void *privdata, int mask);

#ifdef __cplusplus
}
#endif

