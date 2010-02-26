%define selinux_variants mls strict targeted
%define selinux_policyver %(sed -e 's,.*selinux-policy-\\([^/]*\\)/.*,\\1,' /usr/share/selinux/devel/policyhelp)

Name:           cachefilesd
Version:        0.10
Release:        1%{?dist}
Summary:        CacheFiles userspace management daemon
Group:          System Environment/Daemons
License:        GPL
URL:  		http://people.redhat.com/~dhowells/fscache/
Source0:        http://people.redhat.com/dhowells/fscache/cachefilesd-%{version}.tar.bz2

BuildRoot:      %{_tmppath}/%{name}-%{version}-root-%(%{__id_u} -n)
BuildRequires: automake, autoconf
Requires(post): /sbin/chkconfig, /sbin/service
Requires(preun): /sbin/chkconfig, /sbin/service
Requires:       %{name}-selinux = %{version}-%{release}

%description
The cachefilesd daemon manages the caching files and directory that are
that are used by network filesystems such a AFS and NFS to
do persistent caching to the local disk.

%package selinux
Summary:        SELinux policy module supporting cachefilesd
Group:          System Environment/Base
BuildRequires:  checkpolicy, selinux-policy-devel, hardlink
%if "%{selinux_policyver}" != ""
Requires:       selinux-policy >= %{selinux_policyver}
%endif
Requires(post):   /usr/sbin/semodule, /sbin/restorecon
Requires(postun): /usr/sbin/semodule, /sbin/restorecon

%description selinux
SELinux policy module supporting cachefilesd

%prep
%setup -q

%build
%ifarch s390 s390x
PIE="-fPIE"
%else
PIE="-fpie"
%endif
export PIE
CFLAGS="`echo $RPM_OPT_FLAGS $ARCH_OPT_FLAGS $PIE`"

make all

# Build SELinux policy modules
cd selinux
for selinuxvariant in %{selinux_variants}
do
    make NAME=${selinuxvariant} -f /usr/share/selinux/devel/Makefile
    mkdir ${selinuxvariant}
    mv cachefilesd.pp ${selinuxvariant}/cachefilesd.pp
    bzip2 -9 ${selinuxvariant}/cachefilesd.pp
    make NAME=${selinuxvariant} -f /usr/share/selinux/devel/Makefile clean
done
cd -

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/sbin
mkdir -p %{buildroot}%{_sysconfdir}/rc.d/init.d
mkdir -p %{buildroot}%{_mandir}/{man5,man8}
mkdir -p %{buildroot}/usr/share/doc/%{name}-%{version}
mkdir -p %{buildroot}/usr/share/doc/%{name}-selinux-%{version}
mkdir -p %{buildroot}%{_localstatedir}/fscache
make DESTDIR=%{buildroot} install

install -m 644 cachefilesd.conf %{buildroot}%{_sysconfdir}
install -m 755 cachefilesd.initd %{buildroot}%{_sysconfdir}/rc.d/init.d/cachefilesd
install -m 644 selinux/move-cache.txt %{buildroot}/usr/share/doc/%{name}-selinux-%{version}/

# Install SELinux policy modules
cd selinux
for selinuxvariant in %{selinux_variants}
do
    install -d %{buildroot}%{_datadir}/selinux/${selinuxvariant}
    install -p -m 644 ${selinuxvariant}/cachefilesd.pp.bz2 \
           %{buildroot}%{_datadir}/selinux/${selinuxvariant}
done
cd -

# Hardlink identical policy module packages together
/usr/sbin/hardlink -cv %{buildroot}%{_datadir}/selinux

%clean
rm -rf $RPM_BUILD_ROOT

%post
/sbin/chkconfig --add %{name}

if [ "$1" -ge 1 ]; then
	/sbin/service cachefilesd condrestart > /dev/null
fi

%post selinux
# Install SELinux policy modules
for selinuxvariant in %{selinux_variants}
do
  /usr/sbin/semodule -s ${selinuxvariant} -i \
    %{_datadir}/selinux/${selinuxvariant}/cachefilesd.pp.bz2 &> /dev/null || :
done

%preun
if [ $1 -eq 0 ]; then
	/sbin/service cachefilesd stop
	/sbin/chkconfig --del %{name}
fi

%postun
if [ $1 -eq 0 ]; then
	# Fix up non-standard directory context
	/sbin/restorecon -R %{_localstatedir}/fscache || :
fi

%postun selinux
# Clean up after package removal
if [ $1 -eq 0 ]; then
  # Remove SELinux policy modules
  for selinuxvariant in %{selinux_variants}
  do
    /usr/sbin/semodule -s ${selinuxvariant} -r cachefilesd &> /dev/null || :
  done
  # Clean up any remaining file contexts (shouldn't be any really)
  [ -d %{_localstatedir}/fscache ] && \
    /sbin/restorecon -R %{_localstatedir}/fscache &> /dev/null || :
fi

%files
%defattr(-,root,root)
%doc README
%doc howto.txt
%config(noreplace) %{_sysconfdir}/cachefilesd.conf
%attr(0755,root,root) %{_sysconfdir}/rc.d/init.d/cachefilesd
/sbin/*
%{_mandir}/*/*
%{_localstatedir}/fscache

%files selinux
%defattr(-,root,root,0755)
%doc selinux/move-cache.txt
%doc selinux/*.fc
%doc selinux/*.if
%doc selinux/*.te
%{_datadir}/selinux/*/cachefilesd.pp.bz2

%changelog

* Thu Feb 25 2010 David Howells <dhowells@redhat.com>
- Fix the SELinux policies for cachefilesd.
- Compress the installed policy files.

* Tue Feb 23 2010 David Howells <dhowells@redhat.com>
- Must include sys/stat.h to use stat() and co. [RH BZ 565135].
- Remove tail comments from functions.

* Thu Aug 9 2007 David Howells <dhowells@redhat.com> 0.9-1
- The cachefiles module no longer accepts directory fds on cull and inuse
  commands, but rather uses current working directory.

* Mon Jul 2 2007 David Howells <dhowells@redhat.com> 0.8-16
- Use stat64/fstatat64 to avoid EOVERFLOW errors from the kernel on large files.

* Tue Nov 15 2006 David Howells <dhowells@redhat.com> 0.8-15
- Made cachefilesd ask the kernel whether cullable objects are in use and omit
  them from the cull table if they are.
- Made the size of cachefilesd's culling tables configurable.
- Updated the manual pages.

* Mon Nov 14 2006 David Howells <dhowells@redhat.com> 0.8-14
- Documented SELinux interaction.

* Fri Nov 10 2006 David Howells <dhowells@redhat.com> 0.8-11
- Include SELinux policy for cachefilesd.

* Thu Oct 19 2006 Steve Dickson <steved@redhat.com> 0.7-3
- Fixed typo that was causing the howto.txt not to be installed.

* Tue Oct 17 2006 David Howells <dhowells@redhat.com> 0.8-1
- Use /dev/cachefiles if it present in preference to /proc/fs/cachefiles.
- Use poll rather than SIGURG on /dev/cachefilesd.

* Sun Oct 01 2006 Jesse Keating <jkeating@redhat.com> - 0.7-2
- rebuilt for unwind info generation, broken in gcc-4.1.1-21

* Fri Sep 22 2006 Steve Dickson <steved@redhat.com> 0.7-1
- updated to 0.7 which adds the howto.txt

* Wed Aug 30 2006 Steve Dickson <steved@redhat.com> 0.6-1
- Fixed memory corruption problem
- Added the fcull/fstop/frun options

* Fri Aug 11 2006 Steve Dickson <steved@redhat.com> 0.5-1
- Upgraded to 0.5 which fixed initial scan problem when
  started on an empty cache (bz 202184)

* Tue Aug  8 2006 Steve Dickson <steved@redhat.com> 0.4-3
- Updated init.d script to look for cachefilesd in /sbin
- Added postun and preun rules so cachefilesd is stopped
  and started when the rpm is updated or removed.

* Tue Aug  7 2006 Jesse Keating <jkeating@redhat.com> 0.4-2
- require /sbin/chkconfig not /usr/bin/chkconfig

* Tue Aug  1 2006 David Howells <dhowells@redhat.com> 0.4-1
- Discard use of autotools

* Tue Aug  1 2006 Steve Dickson <steved@redhat.com> 0.3-3
- Added URL to source file

* Fri Jul 28 2006 Steve Dickson <steved@redhat.com> 0.3-2
- Added post and preun rules
- Changed init.d script to up right before portmapper.

* Fri Jun  9 2006 Steve Dickson <steved@redhat.com> 0.3-1
- Incorporated David Howells manual page updates

* Thu Jun  8 2006 Steve Dickson <steved@redhat.com> 0.2-1
- Made the daemon 64-bit application.
- Changed the syslog logging to log the daemon's PID
- Changed OS error logging to log errno number as well the string

* Sat Apr 22 2006 Steve Dickson <steved@redhat.com> 0.1-1
- Initial commit
