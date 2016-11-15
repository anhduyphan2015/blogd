#include "server.h"
#include "../deps/h3/include/h3.h"
#include "../deps/http-parser/http_parser.h"
#include "../deps/blogd/helper.h"
#include "../deps/blogd/tinydir.h"
#include "../deps/blogd/regx.h"

#include <hiredis.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

httpMime httpMimes[] = {
    {"gif", "image/gif" },
    {"jpg", "image/jpeg"},
    {"jpeg","image/jpeg"},
    {"png", "image/png" },
    {"htm", "text/html; charset=UTF-8" },
    {"html","text/html; charset=UTF-8" },
    {"js","application/javascript"     },
    {"css","text/css"   },
    {"woff","application/font-woff"   },
    {"woff2","application/font-woff2"   },
    {"ttf","application/octet-stream"   },
    {0, 0}
};

httpRoute httpRoutes[] = {
    {"^/$", responseHttpIndex},
    {"(.*?)\\.(gif|jpg|jpeg|png|htm|html|js|css|woff|woff2|ttf)", responseHttpFile},
    {"/page/(\\d)+", responseHttpPage},
    {"/(.*)", responseHttpContent}
};

/* ============================ Helpers  ======================== */
int formatRedisCommand(char **cmd, int argc, char **argv) {
    size_t *argvlen;
    int j;

    /* Setup argument length */
    argvlen = zmalloc(argc * sizeof(size_t));
    for (j = 0; j < argc; j++)
        argvlen[j] = sdslen(argv[j]);

    return redisFormatCommandArgv(cmd, argc, (const char**) argv, argvlen);
}

int buildRedisCommand(char **cmd, char *argvs[], int argc) {
    char **argv = zmalloc(sizeof(char*) * argc);

    int j;

    for (j = 0; j < argc; j++) {
        argv[j] = sdsnew(argvs[j]);
    }

    return formatRedisCommand(cmd, argc, convertToSds(argc, argv));
}

static struct client *createFakeClient(void) {
    struct client *c = zmalloc(sizeof(*c));

    selectDb(c, 0);
    c->fd = -1;
    c->name = NULL;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->argc = 0;
    c->argv = NULL;
    c->bufpos = 0;
    c->flags = 0;
    c->btype = BLOCKED_NONE;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    c->watched_keys = listCreate();
    c->peerid = NULL;
    listSetFreeMethod(c->reply,decrRefCountVoid);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

static void freeFakeClientArgv(struct client *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);
}

void executeRedisCommand(char **argvs, unsigned int argc) {
    struct client *fakeClient = createFakeClient();

    struct redisCommand *cmd;
    robj **argv;
    argv = zmalloc(sizeof(robj*) * argc);

    fakeClient->argc = argc;
    fakeClient->argv = argv;

    unsigned int i;

    for (i = 0; i < argc; i++) {
        sds argsds = sdsnewlen(NULL, strlen(argvs[i]));
        sdscpy(argsds, argvs[i]);
        argv[i] = createObject(OBJ_STRING, argsds);
    }

    /* Command lookup */
    cmd = lookupCommand(argv[0]->ptr);

    /* Run the command in the context of a fake client */
    cmd->proc(fakeClient);

    /* Clean up. Command code may have changed argv/argc so we use the
     * argv/argc of the client instead of the local variables. */
    freeFakeClientArgv(fakeClient);

    zfree(fakeClient);
    fakeClient = NULL;
}

void callRedisCommand(void *cl, int readlen, size_t qblen, char **argvs, int argc) {
    char *cmd;
    int nread = buildRedisCommand(&cmd, argvs, argc);

    client *c = (client*) cl;

    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);

    strcpy(c->querybuf + qblen, cmd);

    sdsIncrLen(c->querybuf, nread);

    c->lastinteraction = server.unixtime;

    if (c->flags & CLIENT_MASTER) c->reploff += nread;

    server.stat_net_input_bytes += nread;

    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        serverLog(LL_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClient(c);
        return;
    }

    processInputBuffer(c);
}

/* ============================ Redis Commands  ======================== */
static robj *lookupRedisKeyReadOrReply(client *c, robj *key) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) return NULL;
    return o;
}

int getRedisNoReplyCommand(void *cl) {
    robj *o;
    client *c = (client*) cl;

    if ((o = lookupRedisKeyReadOrReply(c,c->argv[1])) == NULL) {
        c->command_last_reply = NULL;
        c->command_last_error = "Not found";
        return C_OK;
    }

    if (o->type != OBJ_STRING) {
        c->command_last_reply = NULL;
        c->command_last_error = shared.wrongtypeerr->ptr;
        return C_ERR;
    } else {
        c->command_last_error = NULL;
        c->command_last_reply = o->ptr;
        return C_OK;
    }
}

/* ============================ Init contents  ======================== */
void initContents(char *content_dir) {
    char *contentPath = stringConcat(content_dir, "/posts/");

    char *layoutFilePath = stringConcat(content_dir, "/layout.tpl");
    char *headerFilePath = stringConcat(content_dir, "/header.tpl");
    char *contentTopFilePath = stringConcat(content_dir, "/content_top.tpl");
    char *footerFilePath = stringConcat(content_dir, "/footer.tpl");
    char *postFilePath = stringConcat(content_dir, "/post.tpl");
    char *pageFilePath = stringConcat(content_dir, "/page.tpl");

    char *error400FilePath = stringConcat(content_dir, "/errors/400.tpl");
    char *error404FilePath = stringConcat(content_dir, "/errors/404.tpl");
    char *error500FilePath = stringConcat(content_dir, "/errors/500.tpl");

    char *postsContents = "";
    char *layoutContent = readFileContent(layoutFilePath);
    char *headerContent = readFileContent(headerFilePath);
    char *topContent = readFileContent(contentTopFilePath);
    char *footerContent = readFileContent(footerFilePath);
    char *postContent = readFileContent(postFilePath);
    char *pageContent = readFileContent(pageFilePath);

    char *error400Content = readFileContent(error400FilePath);
    char *error404Content = readFileContent(error404FilePath);
    char *error500Content = readFileContent(error500FilePath);
    
    if (!layoutContent || !headerContent || !topContent || !footerContent || !postContent || !pageContent) {
        serverLog(LL_WARNING, "PLEASE CHECK CONTENTS DIRECTORY PATH: There is not enough files to run");
        exit(1);
    }

    if (!error400Content || !error404Content || !error500Content) {
        serverLog(LL_WARNING, "PLEASE CHECK ERRORS DIRECTORY PATH: There is not enough files to run");
        exit(1);
    }

    layoutContent = strReplace("{{ include header }}", headerContent, layoutContent);
    layoutContent = strReplace("{{ include content_top }}", topContent, layoutContent);
    layoutContent = strReplace("{{ include footer }}", footerContent, layoutContent);

    // Init 400 error page
    compiledObj *obj400 = compileTemplate(error400Content, layoutContent, server.markdown_compile, NULL);
    char *argvs400[] = {"set", stringConcat(PAGE_ERROR_KEY_PREFIX, "400"), obj400->compiled_content};
    executeRedisCommand(argvs400, 3);
    zfree(obj400); obj400 = NULL;

    // Init 404 error page
    compiledObj *obj404 = compileTemplate(error404Content, layoutContent, server.markdown_compile, NULL);
    char *argvs404[] = {"set", stringConcat(PAGE_ERROR_KEY_PREFIX, "404"), obj404->compiled_content};
    executeRedisCommand(argvs404, 3);
    zfree(obj404); obj404 = NULL;

    // Init 500 error page
    compiledObj *obj500 = compileTemplate(error500Content, layoutContent, server.markdown_compile, NULL);
    char *argvs500[] = {"set", stringConcat(PAGE_ERROR_KEY_PREFIX, "500"), obj500->compiled_content};
    executeRedisCommand(argvs500, 3);
    zfree(obj500); obj500 = NULL;

    unsigned int i, j = 0, numFiles = 0, pageIndex = 1;

    tinydir_dir dir;
    tinydir_open_sorted(&dir, contentPath);

    for (i = 0; i < dir.n_files; i++) {
        tinydir_file file;
        tinydir_readfile_n(&dir, &file, i);

        if (!file.is_dir) {
            numFiles++;
        }
    }

    for (i = 0; i < dir.n_files; i++) {
        tinydir_file file;
        tinydir_readfile_n(&dir, &file, i);

        if (!file.is_dir) {
            char *fileContent = readFileContent(file.path);

            if (fileContent) {
                serverLog(LL_NOTICE, "Compile file '%s'...", file.name);

                // Compiled file name
                char *fileName = removeFileExt(file.name, '.', '/');

                // Compile template
                compiledObj *obj = compileTemplate(fileContent, layoutContent, server.markdown_compile, NULL);

                // Save content
                char *argvs[] = {"set", stringConcat(POST_KEY_PREFIX, fileName), obj->compiled_content};
                executeRedisCommand(argvs, 3);

                // Compile post content
                char *postCompiledContent = strReplace("{{ title }}", obj->title, postContent);
                postCompiledContent = strReplace("{{ thumbnail }}", obj->thumbnail, postCompiledContent);
                postCompiledContent = strReplace("{{ description }}", obj->desc, postCompiledContent);
                postCompiledContent = strReplace("{{ published_at }}", obj->published_at, postCompiledContent);
                postCompiledContent = strReplace("{{ link }}", stringConcat("/", fileName), postCompiledContent);

                postsContents = stringConcat(postsContents, postCompiledContent);

                zfree(obj->compiled_content); obj->compiled_content = NULL;
                zfree(obj); obj = NULL;

                if (((j % server.per_page) == (server.per_page - 1)) || (j == (numFiles - 1))) {
                    // Re-assign index content
                    char *pageCompiledContent = strReplace("{{ posts }}", postsContents, pageContent);

                    char pageNumString[10];
                    sprintf(pageNumString, "%d", pageIndex + 1);

                    if (j < (numFiles - 1)) {
                        char *moreHtml = "<ul class='pager'><li class='next'><a href='/page/{pageNum}'>More</a></li></ul>";

                        moreHtml = strReplace("{pageNum}", pageNumString, moreHtml);

                        pageCompiledContent = strReplace("{{ more }}", moreHtml, pageCompiledContent);
                    } else {
                        pageCompiledContent = strReplace("{{ more }}", "", pageCompiledContent);
                    }

                    sprintf(pageNumString, "%d", pageIndex);

                    // Compile template
                    compiledObj *obj = compileTemplate(pageCompiledContent, layoutContent, server.markdown_compile, NULL);

                    char *argvs[] = {"set", stringConcat(PAGE_KEY_PREFIX, pageNumString), obj->compiled_content};
                    executeRedisCommand(argvs, 3);

                    zfree(postsContents);
                    postsContents = "";
                    pageIndex++;

                    zfree(obj->compiled_content); obj->compiled_content = NULL;
                    zfree(obj); obj = NULL;
                    zfree(pageCompiledContent); pageCompiledContent = NULL;
                }

                sdsfree(fileContent); fileContent = NULL;
                zfree(fileName); fileName = NULL;
                zfree(postCompiledContent); postCompiledContent = NULL;

                j++;
            } else {
                printf("Fail to open file '%s'", file.path);
            }
        }
    }

    tinydir_close(&dir);

    zfree(contentPath); contentPath = NULL;
    zfree(layoutFilePath); layoutFilePath = NULL;
    zfree(headerFilePath); headerFilePath = NULL;
    zfree(contentTopFilePath); contentTopFilePath = NULL;
    zfree(footerFilePath); footerFilePath = NULL;
    zfree(postFilePath); postFilePath = NULL;
    zfree(pageFilePath); pageFilePath = NULL;
    zfree(error400FilePath); error400FilePath = NULL;
    zfree(error404FilePath); error404FilePath = NULL;
    zfree(error500FilePath); error500FilePath = NULL;
    zfree(layoutContent); layoutContent = NULL;

    sdsfree(headerContent); headerContent = NULL;
    sdsfree(topContent); topContent = NULL;
    sdsfree(footerContent); footerContent = NULL;
    sdsfree(postContent); postContent = NULL;
    sdsfree(pageContent); pageContent = NULL;
    sdsfree(error400Content); error400Content = NULL;
    sdsfree(error404Content); error404Content = NULL;
    sdsfree(error500Content); error500Content = NULL;
}

/* ============================ Http response callbacks  ======================== */
void responseHttpIndex(void *cl, char **matches, int readlen, size_t qblen) {
    char *argvs[] = {"getNoReplyCommand", stringConcat(PAGE_KEY_PREFIX, "1")};
    int argc = sizeof(argvs) / sizeof(char*);

    client *c = (client*) cl;
    callRedisCommand(c, readlen, qblen, argvs, argc);

    if (c->command_last_error) {
        responseHttpError(c, readlen, qblen, 404);
    } else {
        responseHttp(c, c->command_last_reply, "html", 200);
    }
}

void responseHttpPage(void *cl, char **matches, int readlen, size_t qblen) {
    char *argvs[] = {"getNoReplyCommand", stringConcat(PAGE_KEY_PREFIX, matches[0])};
    int argc = sizeof(argvs) / sizeof(char*);

    client *c = (client*) cl;
    callRedisCommand(c, readlen, qblen, argvs, argc);

    if (c->command_last_error) {
        responseHttpError(c, readlen, qblen, 404);
    } else {
        responseHttp(c, c->command_last_reply, "html", 200);
    }
}

void responseHttpContent(void *cl, char **matches, int readlen, size_t qblen) {
    char *argvs[] = {"getNoReplyCommand", stringConcat(POST_KEY_PREFIX, matches[0])};
    int argc = sizeof(argvs) / sizeof(char*);

    client *c = (client*) cl;
    callRedisCommand(c, readlen, qblen, argvs, argc);

    if (c->command_last_error) {
        responseHttpError(c, readlen, qblen, 404);
    } else {
        responseHttp(c, c->command_last_reply, "html", 200);
    }
}

void responseHttpError(void *cl, int readlen, size_t qblen, int code) {
    char errorCode[10];

    sprintf(errorCode, "%d", code);

    char *argvs[] = {"getNoReplyCommand", stringConcat(PAGE_ERROR_KEY_PREFIX, errorCode)};
    int argc = sizeof(argvs) / sizeof(char*);

    client *c = (client*) cl;
    callRedisCommand(c, readlen, qblen, argvs, argc);

    responseHttp(c, c->command_last_reply ? c->command_last_reply : "", "html", code);
}

void responseHttpFile(void *cl, char **matches, int readlen, size_t qblen) {
    char *filePath;

    filePath = sdsnew((const char*) server.public_dir);
    filePath = sdscat(filePath, matches[0]);
    filePath = sdscat(filePath, ".");
    filePath = sdscat(filePath, matches[1]);

    client *c = (client*) cl;

    char buff[BUFSIZ];
    struct stat statbuf;
    int fd, readLength;

    fd = open(filePath, O_RDONLY);

    if (fd == -1) {
        responseHttpError(c, readlen, qblen, 404);
        return;
    }

    fstat(fd, &statbuf);

    c->headers = buildHttpHeaders(matches[1], statbuf.st_size, 200);

    addReplyString(c, (const char*) c->headers, sdslen(c->headers));

    while ((readLength = read(fd, buff, sizeof(buff)))) {
        addReplyString(c, buff, readLength);
    }

    close(fd);
}

void responseHttp(void *cl, char *content, char *contentType, unsigned int code) {
    client *c = (client*) cl;

    c->headers = buildHttpHeaders(contentType, strlen(content), code);

    addReplyString(c, (const char*) c->headers, sdslen(c->headers));
    addReplyString(c, (const char*) content, strlen(content));
}

sds *buildHttpHeaders(char *contentType, unsigned int contentLength, unsigned int code) {
    unsigned int i, length;
    char *ext;
    char *headers;

    headers = sdsnew("HTTP/1.1 ");
    ext = (char *)0;

    // Find mime type
    for (i = 0; httpMimes[i].ext != 0; i++) {
        length = strlen(httpMimes[i].ext);

        if (!strncmp(contentType, httpMimes[i].ext, length)) {
            ext = httpMimes[i].filetype;
            break;
        }
    }

    if (ext == 0) {
        code = 400;
    }

    switch (code) {
        case 500:
            ext = "text/html";
            headers = sdscat(headers, "500 Internal Server Error\r\n");
            break;

        case 404:
            ext = "text/html";
            headers = sdscat(headers, "404 Not Found\r\n");
            break;

        case 400:
            ext = "text/html";
            headers = sdscat(headers, "400 Bad Request\r\n");
            break;

        default:
            headers = sdscat(headers, "200 OK\r\n");
            break;
    }

    headers = sdscat(headers, "Server: Blogd\r\n");
    headers = sdscat(headers, "Connection: close\r\n");
    headers = sdscat(headers, "Cache-Control: no-store, must-revalidate\r\n");
    headers = sdscat(headers, "Pragma: no-cache\r\n");
    headers = sdscat(headers, "Expires: 0\r\n");
    headers = sdscat(headers, "x-content-type-options:nosniff\r\n");
    headers = sdscat(headers, "x-frame-options:SAMEORIGIN\r\n");
    headers = sdscat(headers, "x-xss-protection:1; mode=block\r\n");

    /* Content length */
    char contentLengthString[10];
    sprintf(contentLengthString, "%u", contentLength);

    headers = sdscat(headers, "Content-length: ");
    headers = sdscat(headers, contentLengthString);
    headers = sdscat(headers, "\r\n");

    headers = sdscat(headers, "Content-Type: ");
    headers = sdscat(headers, ext);

    headers = sdscat(headers, "\r\n\r\n");

    return (sds *) headers;
}

/* ============================ Process Http Request  ======================== */
void processHttpRequestFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    int readlen;
    size_t qblen;

    client *c = (client*) privdata;

    UNUSED(el);
    UNUSED(mask);

    readlen = PROTO_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    if (c->reqtype == PROTO_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= PROTO_MBULK_BIG_ARG)
    {
        int remaining = (unsigned)(c->bulklen+2)-sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    qblen = sdslen(c->querybuf);

    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;

    /* Http request */
    c->http_querybuf = sdsMakeRoomFor(c->http_querybuf, readlen);
    int httpRequestLength = read(fd, c->http_querybuf + qblen, readlen);

    if (httpRequestLength == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            serverLog(LL_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (httpRequestLength == 0) {
        serverLog(LL_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }

    sdsIncrLen(c->http_querybuf, httpRequestLength);

    RequestHeader *header;
    header = h3_request_header_new();
    h3_request_header_parse(header, c->http_querybuf, httpRequestLength);

    if (strncmp(header->RequestMethod, "GET ", 4) && strncmp(header->RequestMethod, "get ", 4)) {
        h3_request_header_free(header);
        responseHttp(c, "", "", 400);
        return;
    }

    struct http_parser_url u;
    char* fullUrl = strndup(header->RequestURI, header->RequestURILen);

    // Free header
    h3_request_header_free(header);

    // Parse url
    if (http_parser_parse_url(fullUrl, strlen(fullUrl), 0, &u)) {
        free(fullUrl); fullUrl = NULL;
        responseHttp(c, "", "", 400);
        return;
    }

    char *urlPath = strndup(fullUrl + u.field_data[UF_PATH].off, u.field_data[UF_PATH].len);
    char *urlQuery = strndup(fullUrl + u.field_data[UF_QUERY].off, u.field_data[UF_QUERY].len);

    // Check if it has reload action
    char **matches = preg_match(server.reload_content_query, urlQuery);

    if (matches) {
        initContents(server.content_dir);
    }

    zfree(matches); matches = NULL;

    unsigned int numRoutes = sizeof(httpRoutes) / sizeof(struct httpRoute);
    unsigned int i;
    unsigned int isMatched = 0;

    // Find matched route
    for (i = 0; i < numRoutes; i++) {
        struct httpRoute *r = httpRoutes + i;
        char **matches = preg_match(r->pattern, urlPath);

        if (matches) {
            isMatched = 1;
            r->callback(c, matches, readlen, qblen);
            break;
        }
    }

    zfree(matches); matches = NULL;

    free(fullUrl); fullUrl = NULL;
    free(urlPath); urlPath = NULL;
    free(urlQuery); urlQuery = NULL;

    if (!isMatched) {
        responseHttpError(c, readlen, qblen, 404);
        return;
    }
}