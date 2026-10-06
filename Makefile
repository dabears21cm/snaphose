CC?=gcc
CFLAGS?=-Os -g -Wall -Werror -pedantic
EXTRA_CFLAGS=-fPIC -fanalyzer -march=native -pthread

# because an older compiler might not support everything grumble,grumble
define check_cc_flag
  $(shell echo 'int main() { return 0; }' | $(CC) $(1) -xc - 2>/dev/null && echo $(1))
endef

CFLAGS+=$(foreach flag,$(EXTRA_CFLAGS),$(call check_cc_flag,$(flag)))

SNAPHOSE_LIBS?=-lsystemd -lm -lpthread
PREFIX?=/usr/local
ETC?=/etc
SYSTEMD?=/etc/systemd/system/


BUILD_DIR?=build

.PHONY: clean all install

all: snaphose libsnaphose.so snapsave snapdump


SNAPHOSE_OBJS=$(addprefix $(BUILD_DIR)/, snaphose.o snaphose_common.o ini.o)
LIB_OBJS=$(addprefix $(BUILD_DIR)/,  snaphose_common.o)
SNAPSAVE_OBJS=$(addprefix $(BUILD_DIR)/,  snapsave.o snaphose_common.o ini.o )
SNAPDUMP_OBJS=$(addprefix $(BUILD_DIR)/,  snapdump.o snaphose_common.o )

$(BUILD_DIR):
	mkdir -p $@


$(BUILD_DIR)/%.o: src/%.c src/snaphose.h| $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -o $@ $<


snaphose: $(SNAPHOSE_OBJS)
	$(CC) -o $@ $^ $(SNAPHOSE_LIBS)

snapsave: $(SNAPSAVE_OBJS)
	$(CC) -o $@ $^ $(SNAPHOSE_LIBS)


snapdump: $(SNAPDUMP_OBJS)
	$(CC) -o $@ $^ $(SNAPHOSE_LIBS)

libsnaphose.so: $(LIB_OBJS)
	$(CC) -o $@ -shared $^

clean:
	rm -rf $(BUILD_DIR) snaphose libsnaphose.so snapdump snapsave

install: all
	install -d $(PREFIX)/bin
	install snaphose $(PREFIX)/bin
	install snapsave $(PREFIX)/bin
	install snapdump $(PREFIX)/bin
	install libsnaphose.so $(PREFIX)/lib
	install snaphose.ini $(ETC)
	install snapsave.ini $(ETC)
	sed "s|@@@|$(PREFIX)|" snaphose.service.in > $(SYSTEMD)/snaphose.service
	sed "s|@@@|$(PREFIX)|" snapsave.service.in > $(SYSTEMD)/snapsave.service
	systemctl daemon-reload




