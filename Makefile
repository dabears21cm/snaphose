CC?=gcc
CFLAGS?=-Os -g -Wall -Werror -pedantic -fanalyzer -fPIC
SNAPHOSE_LIBS?=-lsystemd -lm
PREFIX?=/usr/local
ETC?=/etc


BUILD_DIR?=build

.PHONY: clean all install

all: snaphose libsnaphose.so


SNAPHOSE_OBJS=$(addprefix $(BUILD_DIR)/, snaphose.o snaphose_common.o ini.o)
LIB_OBJS=$(addprefix $(BUILD_DIR)/,  snaphose_common.o)
SNAPSAVE_OBJS=$(addprefix $(BUILD_DIR)/,  snapsave.o snaphose_common.o ini.o )

$(BUILD_DIR):
	mkdir -p $@


$(BUILD_DIR)/%.o: src/%.c src/snaphose.h| $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -o $@ $<


snaphose: $(SNAPHOSE_OBJS)
	$(CC) -o $@ $^ $(SNAPHOSE_LIBS)

libsnaphose.so: $(LIB_OBJS)
	$(CC) -o $@ -shared $^

clean:
	rm -rf $(BUILD_DIR) snaphose libsnaphose.so

install: all
	install -d $(PREFIX)/bin
	install snaphose $(PREFIX)/bin
	install libsnaphose.so $(PREFIX)/lib
	install snaphose.ini $(ETC)
