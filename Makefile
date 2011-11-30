CFLAGS		:= -g -O2 -Wall
INSTALL		:= install
DESTDIR		:=
ETCDIR		:= /etc
BINDIR		:= /bin
SBINDIR		:= /sbin
SPECFILE	:= redhat/cachefilesd.spec

LNS		:= ln -sf

###############################################################################
#
# Determine the current package version from the specfile
#
###############################################################################
VERSION		:= $(word 2,$(shell grep "^Version:" $(SPECFILE)))
TARBALL		:= cachefilesd-$(VERSION).tar.bz2

###############################################################################
#
# Guess at the appropriate word size
#
###############################################################################
BUILDFOR	:= $(shell file /usr/bin/make | sed -e 's!.*ELF \(32\|64\)-bit.*!\1!')-bit

ifeq ($(BUILDFOR),32-bit)
CFLAGS		+= -m32
else
ifeq ($(BUILDFOR),64-bit)
CFLAGS		+= -m64
endif
endif

###############################################################################
#
# Build stuff
#
###############################################################################
all: cachefilesd

cachefilesd: cachefilesd.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

###############################################################################
#
# Install everything
#
###############################################################################
MAN5	:= $(DESTDIR)/usr/share/man/man5
MAN8	:= $(DESTDIR)/usr/share/man/man8

install: all
	$(INSTALL) -D cachefilesd $(DESTDIR)$(SBINDIR)/cachefilesd
	$(INSTALL) -D -m 0644 cachefilesd.conf $(DESTDIR)$(ETCDIR)/cachefilesd.conf
	$(INSTALL) -D -m 0644 cachefilesd.conf.5 $(MAN5)/cachefilesd.conf.5
	$(INSTALL) -D -m 0644 cachefilesd.8 $(MAN8)/cachefilesd.8

###############################################################################
#
# Clean up
#
###############################################################################
clean:
	$(RM) cachefilesd
	$(RM) *.o *~
	$(RM) debugfiles.list debugsources.list

distclean: clean
	$(RM) -r rpmbuild $(TARBALL)

###############################################################################
#
# Generate a tarball
#
###############################################################################
$(TARBALL):
	git archive --prefix=cachefilesd-$(VERSION)/ --format tar -o $(TARBALL) HEAD

tarball: $(TARBALL)

###############################################################################
#
# Generate an RPM
#
###############################################################################
SRCBALL	:= rpmbuild/SOURCES/$(TARBALL)

BUILDID	:= .local
dist	:= $(word 2,$(shell grep "%dist" /etc/rpm/macros.dist))
release	:= $(word 2,$(shell grep ^Release: $(SPECFILE)))
release	:= $(subst %{?dist},$(dist),$(release))
release	:= $(subst %{?buildid},$(BUILDID),$(release))
rpmver	:= $(VERSION)-$(release)
SRPM	:= rpmbuild/SRPMS/cachefilesd-$(rpmver).src.rpm

RPMBUILDDIRS := \
	--define "_srcrpmdir $(CURDIR)/rpmbuild/SRPMS" \
	--define "_rpmdir $(CURDIR)/rpmbuild/RPMS" \
	--define "_sourcedir $(CURDIR)/rpmbuild/SOURCES" \
	--define "_specdir $(CURDIR)/rpmbuild/SPECS" \
	--define "_builddir $(CURDIR)/rpmbuild/BUILD" \
	--define "_buildrootdir $(CURDIR)/rpmbuild/BUILDROOT"

RPMFLAGS := \
	--define "buildid $(BUILDID)"

rpm:
	mkdir -p rpmbuild
	chmod ug-s rpmbuild
	mkdir -p rpmbuild/{SPECS,SOURCES,BUILD,BUILDROOT,RPMS,SRPMS}
	git archive --prefix=cachefilesd-$(VERSION)/ --format tar -o $(SRCBALL) HEAD
	rpmbuild -ts $(SRCBALL) --define "_srcrpmdir rpmbuild/SRPMS" $(RPMFLAGS)
	rpmbuild --rebuild $(SRPM) $(RPMBUILDDIRS) $(RPMFLAGS)

rpmlint: rpm
	rpmlint $(SRPM) $(CURDIR)/rpmbuild/RPMS/*/cachefilesd-{,debuginfo-}$(rpmver).*.rpm

###############################################################################
#
# Build debugging
#
###############################################################################
show_vars:
	@echo VERSION=$(VERSION)
	@echo TARBALL=$(TARBALL)
	@echo BUILDFOR=$(BUILDFOR)
