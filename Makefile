#
# httpd2
#

all: src/list.h
	gcc -o httpd2 src/server.c -Wall
    
clean:
	-rm -rf httpd2

install:
	-cp httpd2 /usr/bin/
