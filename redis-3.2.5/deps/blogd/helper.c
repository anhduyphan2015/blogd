#include "helper.h"
#include "regx.h"
#include "../../src/zmalloc.h"
#include "../../src/sds.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>

/* String helpers */
char **convertToSds(int count, char** args) {
    int j;
    char **sds = zmalloc(sizeof(char*)*count);

    for(j = 0; j < count; j++)
        sds[j] = sdsnew(args[j]);

    return sds;
}

char *strReplace(char *rep, char *with, char *orig) {
    char *result; // the return string
    char *ins;    // the next insert point
    char *tmp;    // varies
    int len_rep;  // length of rep (the string to remove)
    int len_with; // length of with (the string to replace rep with)
    int len_front; // distance between rep and end of last rep
    int count;    // number of replacements

    // sanity checks and initialization
    if (!orig && !rep)
        return NULL;
    len_rep = strlen(rep);
    if (len_rep == 0)
        return NULL; // empty rep causes infinite loop during count
    if (!with)
        with = "";
    len_with = strlen(with);

    // count the number of replacements needed
    ins = orig;
    for (count = 0; tmp = strstr(ins, rep); ++count) {
        ins = tmp + len_rep;
    }

    tmp = result = zmalloc(strlen(orig) + (len_with - len_rep) * count + 1);

    if (!result)
        return NULL;

    // first time through the loop, all the variable are set correctly
    // from here on,
    //    tmp points to the end of the result string
    //    ins points to the next occurrence of rep in orig
    //    orig points to the remainder of orig after "end of rep"
    while (count--) {
        ins = strstr(orig, rep);
        len_front = ins - orig;
        tmp = strncpy(tmp, orig, len_front) + len_front;
        tmp = strcpy(tmp, with) + len_with;
        orig += len_front + len_rep; // move to next "end of rep"
    }

    strcpy(tmp, orig);

    return result;
}

char* stringConcat(const char *s1, const char *s2) {
    const size_t len1 = strlen(s1);
    const size_t len2 = strlen(s2);

    char *result = zmalloc(len1+len2+1);//+1 for the zero-terminator
    //in real code you would check for errors in malloc here

    memcpy(result, s1, len1);
    memcpy(result+len1, s2, len2+1);//+1 to copy the null-terminator

    return result;
}

/* File & Directory helpers */
char *removeFileExt(char* mystr, char dot, char sep) {
    char *retstr, *lastdot, *lastsep;

    // Error checks and allocate string.

    if (mystr == NULL)
        return NULL;
    if ((retstr = zmalloc (strlen (mystr) + 1)) == NULL)
        return NULL;

    // Make a copy and find the relevant characters.

    strcpy (retstr, mystr);
    lastdot = strrchr (retstr, dot);
    lastsep = (sep == 0) ? NULL : strrchr (retstr, sep);

    // If it has an extension separator.

    if (lastdot != NULL) {
        // and it's before the extenstion separator.

        if (lastsep != NULL) {
            if (lastsep < lastdot) {
                // then remove it.

                *lastdot = '\0';
            }
        } else {
            // Has extension separator with no path separator.

            *lastdot = '\0';
        }
    }

    return retstr;
}

char *readFileContent(char *path) {
    char *content = sdsempty();
    char buf[1024 + 1];

    FILE * fp = fopen(path, "r");

    if (fp != NULL) {
        while(fgets(buf, 1024 + 1,fp) != NULL)
            content = sdscat(content, buf);

        fclose(fp);
    }

    return (char *) content;
}

void createDir(char *path, mode_t mode) {
    struct stat st = {0};

    if (stat(path, &st) == -1) {
        mkdir(path, mode);
    }
}

