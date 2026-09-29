CC?=gcc
CFLAGS?=-Os -g -Wall -Werror -pedantic -fanalyzer
LDFLAGS?=-lsystemd
PREFIX?=/usr/local
ETC?=/etc


BUILD_DIR?=build

.PHONY: clean all install

all: snaphose


OBJS=$(addprefix $(BUILD_DIR)/, snaphose.o ini.o)

$(BUILD_DIR):
	mkdir -p $@


$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -o $@ $<


snaphose: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)


clean: 
	rm -rf $(BUILD_DIR) snaphose

install: all
	install -d $(PREFIX)/bin
	install snaphose $(PREFIX)/bin
	install snaphose.ini $(ETC)
