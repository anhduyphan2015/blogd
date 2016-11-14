#ifndef BLOGD_H
#define BLOGD_H

#include "ae.h"
#include "sds.h"

#define PAGE_KEY_PREFIX "blogd::page"
#define PAGE_ERROR_KEY_PREFIX "blogd::page::error"
#define POST_KEY_PREFIX "blogd::post::"

typedef void httpRouteCallback(void *cl, char **matches, int readlen, size_t qblen);

typedef struct httpMime {
    char *ext;
    char *filetype;
} httpMime;

typedef struct httpRoute {
    char *pattern;
    httpRouteCallback *callback;
} httpRoute;

/* Redis helpers */
int formatRedisCommand(char **cmd, int argc, char **argv);
int buildRedisCommand(char **cmd, char *argvs[], int argc);
void executeRedisCommand(char **argvs, unsigned int argc);
void callRedisCommand(void *cl, int readlen, size_t qblen, char **argvs, int argc);

/* Redis commands */
int getRedisNoReplyCommand(void *cl);

/* Main */
void initContents(char *content_dir);
void processHttpRequestFromClient(aeEventLoop *el, int fd, void *privdata, int mask);

/* Response */
sds *buildHttpHeaders(char *contentType, unsigned int contentLength, unsigned int code);
void responseHttp(void *cl, char *content, char *contentType, unsigned int code);
void responseHttpIndex(void *cl, char **matches, int readlen, size_t qblen);
void responseHttpPage(void *cl, char **matches, int readlen, size_t qblen);
void responseHttpContent(void *cl, char **matches, int readlen, size_t qblen);
void responseHttpFile(void *cl, char **matches, int readlen, size_t qblen);
void responseHttpError(void *cl, int readlen, size_t qblen, int code);

#endif
