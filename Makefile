CFLAGS		:= -g -O2 -Wall
INSTALL		:= install
DESTDIR		:=
BUILDFOR	:=
ETCDIR		:= /etc
BINDIR		:= /bin
SBINDIR		:= /sbin

LNS		:= ln -sf

ifeq ($(BUILDFOR),32-bit)
CFLAGS		+= -m32
else
ifeq ($(BUILDFOR),64-bit)
CFLAGS		+= -m64
endif
endif

#
# building
#
all: cachefilesd

cachefilesd: cachefilesd.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

#
# installation
#
MAN5	:= $(DESTDIR)/usr/share/man/man5
MAN8	:= $(DESTDIR)/usr/share/man/man8

install: all
	$(INSTALL) -D cachefilesd $(DESTDIR)$(SBINDIR)/cachefilesd
	$(INSTALL) -D -m 0644 cachefilesd.conf $(DESTDIR)$(ETCDIR)/cachefilesd.conf
	$(INSTALL) -D -m 0644 cachefilesd.conf.5 $(MAN5)/cachefilesd.conf.5
	$(INSTALL) -D -m 0644 cachefilesd.8 $(MAN8)/cachefilesd.8

#
# clean up
#
clean:
	$(RM) cachefilesd
	$(RM) *.o *~
	$(RM) debugfiles.list debugsources.list
