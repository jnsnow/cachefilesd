CFLAGS		:= -g -O2 -Wall
INSTALL		:= install
DESTDIR		:=
MAJOR		:= 0
MINOR		:= 4
VERSION		:= $(MAJOR).$(MINOR)
BUILDFOR	:= 
ETCDIR		:= /etc
BINDIR		:= /bin
SBINDIR		:= /sbin
LIBDIR		:= /lib
USRLIBDIR	:= /usr/lib
SHAREDIR	:= /usr/share/keyutils
INCLUDEDIR	:= /usr/include
ARLIB		:= libkeyutils.a
DEVELLIB	:= libkeyutils.so
SONAME		:= libkeyutils.so.$(MAJOR)
LIBNAME		:= libkeyutils-$(VERSION).so

LNS		:= ln -sf

ifeq ($(BUILDFOR),32-bit)
CFLAGS		+= -m32
LIBDIR		:= /lib
USRLIBDIR	:= /usr/lib
else
ifeq ($(BUILDFOR),64-bit)
CFLAGS		+= -m64
LIBDIR		:= /lib64
USRLIBDIR	:= /usr/lib64
endif
endif

all: cachefilesd

cachefilesd: cachefilesd.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

MAN5	:= $(DESTDIR)/usr/share/man/man5
MAN8	:= $(DESTDIR)/usr/share/man/man8

install: all
	$(INSTALL) -D cachefilesd $(DESTDIR)$(SBINDIR)/cachefilesd
	$(INSTALL) -D -m 0644 cachefilesd.conf $(DESTDIR)$(ETCDIR)/cachefilesd.conf
	$(INSTALL) -D -m 0644 cachefilesd.conf.5 $(MAN5)/cachefilesd.conf.5
	$(INSTALL) -D -m 0644 cachefilesd.8 $(MAN8)/cachefilesd.8

clean:
	$(RM) cachefilesd
	$(RM) *.o *~
	$(RM) debugfiles.list debugsources.list
