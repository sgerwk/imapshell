PROGS=imapshell
HELP=imapshell-help.c

CC=gcc

CFLAGS+=-g -Wall -Wextra
imapshell: LDFLAGS+=-lreadline

all: ${PROGS}

imapshell: imapshell-help.c
imapshell: CFLAGS+=-DSSLFT
imapshell: LDFLAGS+=-lcrypto -lssl

%-help.c: %.help
	echo "char *helptext = \"\\" > $@
	sed 's,$$,\\n\\,' $< >> $@
	echo "\";" >> $@

%.html: %
	lowdown $< | \
	sed -e 's,<code>,<tt style="color: brown;">,g' \
	    -e 's,<\/code>,<\/tt>,g' > $@

%.txt: %
	groff -man -Tascii $< > $@
	# less -R $@
	# key R to reload

install: all
	cp imapshell $$HOME/bin/
	cp imapenvelope $$HOME/bin
	cp imapshell.1 $$HOME/man/man1/

clean:
	rm -f ${PROGS} ${MANS} ${HELP} *.o

