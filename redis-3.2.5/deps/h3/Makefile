# CC = gcc
# ifeq ($(shell uname),linux)
# SOURCE_FLAGS = -D_POSIX_SOURCE -D_BSD_SOURCE
# endif

#CFLAGS = -Os -Wall -Wextra -Werror \
#	 $(SOURCE_FLAGS) -g \
#	 -std=gnu99 \
# -DNDEBUG

OBJ =  src/header_field.o src/header_field_list.o src/request_header.o src/hash.o src/mempool.o

CFLAGS += -std=c99 -Iinclude -Isrc -Wall -ggdb -O2
LDFLAGS = -lpthread

LIBH3_A = libh3.a
LIBH3_SO = libh3.so
LIBH3 = $(LIBH3_A) $(LIBH3_SO)
EXECUTABLE = test

all: $(EXECUTABLE) $(LIBH3)

src/%.o: src/%.c
	@$(CC)  -fpic -c -o $@ $< $(CFLAGS)

parser.o: parser.c
	@$(CC) -c -o $@ $< $(CFLAGS)

$(EXECUTABLE): parser.o $(LIBH3_A)
	@gcc -o $(EXECUTABLE) parser.o  $(LIBH3_A) $(CFLAGS) $(LDFLAGS)

$(LIBH3_SO): $(OBJ)
	@gcc -shared -Wl,-soname,$(LIBH3_SO) -o $(LIBH3_SO) $^ $(CFLAGS)

$(LIBH3_A): $(OBJ)
	@ar -cr $@ $^
	@ranlib $@

.PHONY: clean

clean:
	@rm -rf $(EXECUTABLE) parser.o $(OBJ) $(LIBH3)
