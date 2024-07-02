MAKEFLAGS += --jobs=$(shell nproc)

cc := clang
#cc := gcc
#fsan := -fsanitize=address,undefined # Our memory manager breaks it. Use valgrind.

cflags += -Wall -Werror
cflags += -Wno-unused
cflags += -std=c99
cflags += -g3
cflags += -O2
cflags += -MMD -MP
cflags += -MT bin/$*.o
cflags += -o $@
cflags += -DDEBUG
#cflags += -DNDEBUG
cflags += -I/usr/include/postgresql
cflags += $(fsan)

ifeq ($(cc),gcc)
  cflags += -Wno-missing-braces
endif

lflags += -o $@
lflags += $(fsan)
lflags += -lm

# |Cleanup: Surely all this isn't necessary to link with Postgres?
lflags += -L/usr/local/src/postgresql-14.8/build/src/interfaces/libpq
lflags += -L/usr/local/src/postgresql-14.8/build/src/common
lflags += -L/usr/local/src/postgresql-14.8/build/src/port
lflags += -Wl,-Bstatic -lpq -lpgcommon -lpgport -Wl,-Bdynamic
lflags += -lpthread

# Build targets:
all:  bin/main
all:  tags

# Run targets:
#all:  ; bin/main

sources    := $(shell find src -type f)
non_mains  := $(shell grep -L '^int main' $(sources))
shared_obj := $(patsubst src/%.c,bin/%.o,$(filter %.c,$(non_mains)))
deps       := $(patsubst src/%.c,bin/%.d,$(filter %.c,$(sources)))
src_dirs   := $(dir $(sources))

$(shell mkdir -p $(patsubst src%,bin%,$(src_dirs)))


bin/%:  bin/%.o $(shared_obj);  $(cc) $^ $(lflags)

bin/%.o:  src/%.c;  $(cc) -c $(cflags) $<

tags:  $(sources);  ctags --recurse src/

tidy:           ;  rm -f core.* vgcore.*
pgcache-clean:  ;  rm -f /tmp/*.pgcache
clean:      tidy;  rm -rf bin tags

bin/%.d: ;
include $(deps)

# Type-check JavaScript:
bin/checked-%-js:  web/%.js;  (tsc --noEmit --checkJs --target es6 $< && touch $@) | sed 's/(\(.*\),\(.*\)):/:\1:\2:/'
all:  $(patsubst web/%.js,bin/checked-%-js,$(wildcard web/*.js))
