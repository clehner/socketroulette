BIN = socketroulette
SRC = socketroulette.c
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: $(BIN)

$(BIN): $(SRC)

$(BINDIR):
	mkdir -p $@

install: all | $(BINDIR)
	@cp -vf $(BIN) $(BINDIR)

link: all | $(BINDIR)
	@ln -vfrs $(BIN) $(BINDIR)

uninstall unlink:
	@rm -vf $(BINDIR)/$(BIN)

clean:
	rm $(BIN)

.PHONY: all clean install uninstall link unlink
