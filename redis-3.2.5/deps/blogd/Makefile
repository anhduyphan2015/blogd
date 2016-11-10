STD=
WARN= -Wall
OPT= -O2

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
R_LDFLAGS= $(LDFLAGS) -lpcre
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

all: content.o helper.o regx.o tinydir.h

.PHONY: all

content.o: content.h content.c ../sundown/src/markdown.o ../sundown/src/buffer.o ../sundown/src/autolink.o ../sundown/src/stack.o ../sundown/html/html.o ../sundown/html/houdini_href_e.o ../sundown/html/houdini_html_e.o ../sundown/html/html_smartypants.c ../sundown/src/html_blocks.h helper.o regx.o
helper.o: helper.h helper.c
regx.o: regx.h regx.c

.c.o:
	$(R_CC) -c $<

clean:
	rm -f *.o