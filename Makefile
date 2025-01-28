MAKEFLAGS += --jobs=$(shell nproc)

#cc := clang -fsanitize=thread
cc := gcc

cflags += -Wall -Werror
cflags += -Wno-unused
cflags += -std=c99
cflags += -g3
#cflags += -O2
#cflags += -DNDEBUG
cflags += -MMD -MP
cflags += -MT bin/$*.o
cflags += -o $@
cflags += -I/usr/include/postgresql

ifeq ($(cc),gcc)
  cflags += -Wno-missing-braces
endif

lflags += -o $@
lflags += -lm
lflags += -lpthread

# |Cleanup: Surely all this isn't necessary to link with Postgres?
lflags += -L/usr/local/src/postgresql-14.8/build/src/interfaces/libpq
lflags += -L/usr/local/src/postgresql-14.8/build/src/common
lflags += -L/usr/local/src/postgresql-14.8/build/src/port
lflags += -Wl,-Bstatic -lpq -lpgcommon -lpgport -Wl,-Bdynamic

sources    := $(shell find src -type f)
non_mains  := $(shell grep -L '^int main' $(sources))
shared_obj := $(patsubst src/%.c,bin/%.o,$(filter %.c,$(non_mains)))
deps       := $(patsubst src/%.c,bin/%.d,$(filter %.c,$(sources)))
src_dirs   := $(dir $(sources))
exes       := $(patsubst src/%.c,bin/%,$(filter-out $(non_mains),$(sources)))

$(shell mkdir -p $(patsubst src%,bin%,$(src_dirs)))

# Build targets:
all:  $(exes)
all:  tags

# Run targets:
#all:  ; bin/main

bin/%:  bin/%.o $(shared_obj);  $(cc) $^ $(lflags)

bin/%.o:  src/%.c;  $(cc) -c $(cflags) $<

tags:  $(sources);  ctags --recurse src web

tidy:           ;  rm -f core.* vgcore.* gmon.out
pgcache-clean:  ;  rm -f /tmp/*.pgcache
clean:      tidy;  rm -rf bin tags

bin/%.d: ;
include $(deps)

# Type-check JavaScript:
bin/checked-%-js:  web/%.js;  (tsc --noEmit --checkJs --target es6 $< && touch $@) | sed 's/(\(.*\),\(.*\)):/:\1:\2:/'
all:  $(patsubst web/%.js,bin/checked-%-js,$(wildcard web/*.js))
