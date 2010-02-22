Name: cachefilesd
Version: 0.4
Release: 1%{?dist}
Summary: CacheFiles userspace management daemon
Group: System Environment/Daemons
License: GPL
BuildRoot: %{_tmppath}/%{name}-%{version}-root-%(%{__id_u} -n)
Url: http://people.redhat.com/~dhowells/fscache/
Source0: http://people.redhat.com/~dhowells/fscache/cachefilesd-0.4.tar.bz2
Requires(post): /usr/bin/chkconfig
Requires(post): /usr/bin/chkconfig

%description
The cachefilesd daemon manages the caching files and directory that are that
are used by network filesystems such a AFS and NFS to do persistent caching to
the local disk.

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


%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/sbin
mkdir -p %{buildroot}%{_sysconfdir}/rc.d/init.d
mkdir -p %{buildroot}%{_mandir}/{man5,man8}
make DESTDIR=%{buildroot} install

install -m 755 cachefilesd.initd %{buildroot}%{_sysconfdir}/rc.d/init.d/cachefilesd

%clean
rm -rf $RPM_BUILD_ROOT

%post
/sbin/chkconfig --add %{name}

%preun
if [ $1 -eq 0 ]; then
	/sbin/chkconfig --del %{name}
fi


%files
%defattr(-,root,root)
%doc README
%config(noreplace) %{_sysconfdir}/cachefilesd.conf
%attr(0755,root,root) %{_sysconfdir}/rc.d/init.d/cachefilesd
/sbin/*
%{_mandir}/*/*

%changelog
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
