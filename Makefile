PREFIX ?= /usr/local

all:
	g++ main.cpp --std=c++20 -o paetkis

debug:
	g++ main.cpp --std=c++20 -o paetkis -g

install:
	install -Dm755 paetkis $(DESTDIR)$(PREFIX)/bin/paetkis

clean:
	rm -f paetkis

.PHONY: clean install
