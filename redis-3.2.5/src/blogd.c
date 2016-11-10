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

httpMime httpMimes[] = {
    {"gif", "image/gif" },
    {"jpg", "image/jpeg"},
    {"jpeg","image/jpeg"},
    {"png", "image/png" },
    {"htm", "text/html; charset=UTF-8" },
    {"html","text/html; charset=UTF-8" },
    {"js","application/javascript"     },
    {"css","text/css; charset=UTF-8"   },
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
static robj *lookupRedisKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) return NULL;
    return o;
}

int getRedisNoReplyCommand(void *cl) {
    robj *o;
    client *c = (client*) cl;

    if ((o = lookupRedisKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL) {
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

    char *error404FilePath = stringConcat(content_dir, "/errors/404.tpl");
    char *error500FilePath = stringConcat(content_dir, "/errors/500.tpl");

    char *postsContents = "";
    char *layoutContent = readFileContent(layoutFilePath);
    char *headerContent = readFileContent(headerFilePath);
    char *topContent = readFileContent(contentTopFilePath);
    char *footerContent = readFileContent(footerFilePath);
    char *postContent = readFileContent(postFilePath);
    char *pageContent = readFileContent(pageFilePath);

    char *error404Content = readFileContent(error404FilePath);
    char *error500Content = readFileContent(error500FilePath);
    
    if (!layoutContent || !headerContent || !topContent || !footerContent || !postContent || !pageContent) {
        serverLog(LL_WARNING, "PLEASE CHECK CONTENTS DIRECTORY PATH: There is not enough files to run");
        exit(1);
    }

    if (!error404Content || !error500Content) {
        serverLog(LL_WARNING, "PLEASE CHECK ERRORS DIRECTORY PATH: There is not enough files to run");
        exit(1);
    }

    layoutContent = strReplace("{{ include header }}", headerContent, layoutContent);
    layoutContent = strReplace("{{ include content_top }}", topContent, layoutContent);
    layoutContent = strReplace("{{ include footer }}", footerContent, layoutContent);

    // Init 404 error page
    compiledObj *obj404 = compileTemplate(error404Content, layoutContent, NULL, server.markdown_compile);
    char *argvs404[] = {"set", stringConcat(PAGE_ERROR_KEY_PREFIX, "404"), obj404->compiled_content};
    executeRedisCommand(argvs404, 3);
    zfree(obj404);
    obj404 = NULL;

    // Init 500 error page
    compiledObj *obj500 = compileTemplate(error500Content, layoutContent, NULL, server.markdown_compile);
    char *argvs500[] = {"set", stringConcat(PAGE_ERROR_KEY_PREFIX, "500"), obj500->compiled_content};
    executeRedisCommand(argvs500, 3);
    zfree(obj500);
    obj500 = NULL;

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
                printf("Compile file '%s'...\n", file.name);

                // Compiled file name
                char *fileName = removeFileExt(file.name, '.', '/');

                // Compile template
                compiledObj *obj = compileTemplate(fileContent, layoutContent, NULL, server.markdown_compile);

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

                free(fileContent);

                zfree(obj);
                obj = NULL;

                if (((j % server.per_page) == (server.per_page - 1)) || (j == (numFiles - 1))) {
                    // Re-assign index content
                    char *pageCompiledContent = strReplace("{{ posts }}", postsContents, pageContent);

                    char pageNum[10];
                    sprintf(pageNum, "%d", pageIndex + 1);

                    if (j < (numFiles - 1)) {
                        char *moreHtml = "<a href='/page/{pageNum}'>More</a>";

                        moreHtml = strReplace("{pageNum}", pageNum, moreHtml);

                        pageCompiledContent = strReplace("{{ more }}", moreHtml, pageCompiledContent);
                    } else {
                        pageCompiledContent = strReplace("{{ more }}", "", pageCompiledContent);
                    }

                    sprintf(pageNum, "%d", pageIndex);

                    // Compile template
                    compiledObj *obj = compileTemplate(pageCompiledContent, layoutContent, NULL, server.markdown_compile);

                    char *argvs[] = {"set", stringConcat(PAGE_KEY_PREFIX, pageNum), obj->compiled_content};
                    executeRedisCommand(argvs, 3);

                    postsContents = "";
                    pageIndex++;

                    zfree(obj);
                    obj = NULL;
                }

                j++;
            } else {
                printf("Fail to open file '%s'", file.path);
            }
        }
    }

    tinydir_close(&dir);
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
        responseHttp(c, c->command_last_reply, "html", 200, 0);
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
        responseHttp(c, c->command_last_reply, "html", 200, 0);
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
        responseHttp(c, c->command_last_reply, "html", 200, 0);
    }
}

void responseHttpError(void *cl, int readlen, size_t qblen, int code) {
    char errorCode[10];

    sprintf(errorCode, "%d", code);

    char *argvs[] = {"getNoReplyCommand", stringConcat(PAGE_ERROR_KEY_PREFIX, errorCode)};
    int argc = sizeof(argvs) / sizeof(char*);

    client *c = (client*) cl;
    callRedisCommand(c, readlen, qblen, argvs, argc);

    responseHttp(c, c->command_last_reply ? c->command_last_reply : "", "html", code, 0);
}

void responseHttpFile(void *cl, char **matches, int readlen, size_t qblen) {
    char *filePath = stringConcat(server.public_dir, matches[0]);
    filePath = stringConcat(filePath, ".");
    filePath = stringConcat(filePath, matches[1]);

    client *c = (client*) cl;

    int fileFd = open(filePath, O_RDONLY);
    unsigned char buffer[PROTO_REPLY_CHUNK_BYTES];
    long ret;

    if (fileFd == -1) {
        responseHttpError(c, readlen, qblen, 404);
    } else {
        char *content = buildHttpHeaders("", matches[1], 200, 1);

        //write(c->fd, content, strlen(content));

        while (1) {
            // Read data into buffer.  We may not have enough to fill up buffer, so we
            // store how many bytes were actually read in bytes_read.
            int bytes_read = read(fileFd, buffer, sizeof(buffer));
            if (bytes_read == 0) // We're done reading from the file
                break;

            if (bytes_read < 0) {
                // handle errors
            }

            // You need a loop for the write, because not all of the data may be written
            // in one call; write will return how many bytes were written. p keeps
            // track of where in the buffer we are, while we decrement bytes_read
            // to keep track of how many bytes are left to write.
            void *p = buffer;
            while (bytes_read > 0) {
                int bytes_written = send(c->fd, p, bytes_read, 0);
                if (bytes_written <= 0) {
                    // handle errors
                }
                bytes_read -= bytes_written;
                p += bytes_written;
            }
        }

        /*while (ret = read(fileFd, buffer, PROTO_REPLY_CHUNK_BYTES)) {
            write(c->fd, buffer, ret);
        }*/

        //close(fileFd);
    }

    //freeClient(c);
}

char *buildHttpHeaders(char *content, char *contentType, unsigned int code, unsigned int isChunk) {
    unsigned int i, length;
    char *ext;
    char *res = "HTTP/1.1 ";

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
            if (!content) {
                content = "500 Internal Server Error";
            }

            ext = "text/html";
            res = stringConcat(res, "500 Internal Server Error\r\n");
            break;

        case 404:
            if (!content) {
                content = "404 Not Found";
            }

            ext = "text/html";
            res = stringConcat(res, "404 Not Found\r\n");
            break;

        case 400:
            if (!content) {
                content = "Bad Request";
            }

            ext = "text/html";
            res = stringConcat(res, "400 Bad Request\r\n");
            break;

        default:
            res = stringConcat(res, "200 OK\r\n");
            break;
    }

    res = stringConcat(res, "Server: Blogd\r\n");
    res = stringConcat(res, "Connection: close\r\n");
    res = stringConcat(res, "Cache-Control: no-store, must-revalidate\r\n");
    res = stringConcat(res, "Pragma: no-cache\r\n");
    res = stringConcat(res, "Expires: 0\r\n");
    res = stringConcat(res, "x-content-type-options:nosniff\r\n");
    res = stringConcat(res, "x-frame-options:SAMEORIGIN\r\n");
    res = stringConcat(res, "x-xss-protection:1; mode=block\r\n");

    if (!isChunk) {
        char contentLength[10];
        sprintf(contentLength, "%zu", strlen(content));

        res = stringConcat(res, "Content-length: ");
        res = stringConcat(res, contentLength);
        res = stringConcat(res, "\r\n");
    }

    res = stringConcat(res, "Content-Type: ");
    res = stringConcat(res, ext);

    res = stringConcat(res, "\r\n\r\n");

    return res;
}

void responseHttp(void *cl, char *content, char *contentType, unsigned int code, unsigned int isChunk) {
    client *c = (client*) cl;

    char *res = buildHttpHeaders(content, contentType, code, isChunk);
    res = stringConcat(res, content);

    addReplyString(c, res, strlen(res));
}

/* ============================ Process Http Request  ======================== */
void processHttpRequestFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    int readlen;
    size_t qblen;
    sds httpRequestBuff = sdsempty();

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
    httpRequestBuff = sdsMakeRoomFor(httpRequestBuff, readlen);
    int httpRequestLength = read(fd, httpRequestBuff + qblen, readlen);

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

    sdsIncrLen(httpRequestBuff, httpRequestLength);

    RequestHeader *header;
    header = h3_request_header_new();
    h3_request_header_parse(header, httpRequestBuff, httpRequestLength);

    if (strncmp(header->RequestMethod, "GET ", 4) && strncmp(header->RequestMethod, "get ", 4)) {
        responseHttp(c, "", "", 400, 0);
        return;
    }

    struct http_parser_url u;
    char* fullUrl = strndup(header->RequestURI, header->RequestURILen);

    // Parse url
    if (http_parser_parse_url(fullUrl, strlen(fullUrl), 0, &u)) {
        responseHttp(c, "", "", 400, 0);
        return;
    }

    char *urlPath = strndup(fullUrl + u.field_data[UF_PATH].off, u.field_data[UF_PATH].len);
    char *urlQuery = strndup(fullUrl + u.field_data[UF_QUERY].off, u.field_data[UF_QUERY].len);

    // Check if it has reload action
    char **matches = preg_match(server.reload_content_query, urlQuery);

    if (matches) {
        initContents(server.content_dir);
    }

    int numRoutes = sizeof(httpRoutes) / sizeof(struct httpRoute);
    unsigned int i;
    bool isMatched = false;

    // Find matched route
    for (i = 0; i < numRoutes; i++) {
        struct httpRoute *r = httpRoutes + i;
        char **matches = preg_match(r->pattern, urlPath);

        if (matches) {
            isMatched = true;
            r->callback(c, matches, readlen, qblen);
            break;
        }
    }

    h3_request_header_free(header);

    if (!isMatched) {
        responseHttpError(c, readlen, qblen, 404);
        return;
    }
}