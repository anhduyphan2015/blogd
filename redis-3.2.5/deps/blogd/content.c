#include "content.h"
#include "helper.h"
#include "regx.h"
#include "tinydir.h"
#include "../sundown/src/markdown.h"
#include "../sundown/src/buffer.h"
#include "../sundown/html/html.h"
#include "../../src/zmalloc.h"
#include "../../src/sds.h"

struct buf *compileMarkdownContent(char *content) {
    struct buf *ob;
    struct sd_callbacks callbacks;
    struct html_renderopt options;
    struct sd_markdown *markdown;

    ob = bufnew(OUTPUT_UNIT);

    sdhtml_renderer(&callbacks, &options, 0);
    markdown = sd_markdown_new(0, 16, &callbacks, &options);

    sd_markdown_render(ob, (const uint8_t *) content, strlen(content), markdown);
    sd_markdown_free(markdown);

    return ob;
}

compiledObj *compileTemplate(char *fileContent, char *layoutContent, int markdownCompile, char *path) {
    // Regx match
    char **titleMatches = preg_match("@section_title\\s*((.|\\n)*?)\\s*@endsection", fileContent);
    char **metaDescMatches = preg_match("@section_meta_description\\s*((.|\\n)*?)\\s*@endsection", fileContent);
    char **descMatches = preg_match("@section_description\\s*((.|\\n)*?)\\s*@endsection", fileContent);
    char **thumbnailMatches = preg_match("@section_thumbnail\\s*((.|\\n)*?)\\s*@endsection", fileContent);
    char **contentMatches = preg_match("@section_content\\s*((.|\\n)*?)\\s*@endsection", fileContent);
    char **publishedAtMatches = preg_match("@section_published_at\\s*((.|\\n)*?)\\s*@endsection", fileContent);

    compiledObj *obj = zmalloc(sizeof(compiledObj));
    struct buf *ob;
    char *content = "";

    // Assign data
    obj->title = titleMatches ? titleMatches[0] : "";
    obj->meta_desc = metaDescMatches ? metaDescMatches[0] : "";
    obj->desc = descMatches ? descMatches[0] : "";
    obj->thumbnail = thumbnailMatches ? thumbnailMatches[0] : "";
    obj->published_at = publishedAtMatches ? publishedAtMatches[0] : "";

    if (contentMatches) {
        if (markdownCompile > 0) {
            ob = compileMarkdownContent(contentMatches[0]);
            content = (char *) ob->data;
        } else {
            content = contentMatches[0];
        }
    }

    // Complied content
    obj->compiled_content = "";
    obj->compiled_content = strReplace("{{ title }}", obj->title, layoutContent);
    obj->compiled_content = strReplace("{{ meta_description }}", obj->meta_desc, obj->compiled_content);
    obj->compiled_content = strReplace("{{ content }}", content, obj->compiled_content);

    if (contentMatches && (markdownCompile > 0)) {
        bufrelease(ob);
    }

    // Write complied content
    if (path) {
        FILE *fp = fopen(path, "w+");
        fputs(obj->compiled_content, fp);
        fclose(fp);
    }

    return obj;
}