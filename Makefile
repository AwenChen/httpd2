#
# httpd2 makefile
#

EXEC = httpd2

CFLAGS = -Wall
CFLAGS += -DCONFIG_SCHE_RR
CFALGS += -I./

all:
	gcc $(CFLAGS) -o $(EXEC) src/server.c src/list.h

clean:
	-rm -rf httpd2

install:
	-cp httpd2 /usr/local/bin/