#ifndef BLOGD_CONTENT_H
#define BLOGD_CONTENT_H

#define OUTPUT_UNIT 64

typedef struct compiledObj {
    char *title;
    char *meta_desc;
    char *desc;
    char *thumbnail;
    char *published_at;
    char *compiled_content;
} compiledObj;

struct buf *compileMarkdownContent(char *content);
compiledObj *compileTemplate(char *fileContent, char *layoutContent, char *path, int markdownCompile);

#endif
