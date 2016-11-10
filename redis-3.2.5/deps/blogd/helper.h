#ifndef BLOGD_UTIL_H
#define BLOGD_UTIL_H

#include <sys/types.h>

char **convertToSds(int count, char** args);
char *strReplace(char *rep, char *with, char *orig);
char* stringConcat(const char *s1, const char *s2);
char *removeFileExt(char* mystr, char dot, char sep);
char *readFileContent(char *path);
void createDir(char *path, mode_t mode);

#endif
