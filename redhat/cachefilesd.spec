Name:           cachefilesd
Version:        0.7
Release:        1%{?dist}
Summary:        CacheFiles userspace management daemon
Group:          System Environment/Daemons
License:        GPL
URL:  			http://people.redhat.com/~dhowells/fscache/
Source0:        http://people.redhat.com/dhowells/fscache/cachefilesd-0.6.tar.bz2

BuildRoot:      %{_tmppath}/%{name}-%{version}-root-%(%{__id_u} -n)
BuildRequires: automake, autoconf
Requires(post): /sbin/chkconfig, /sbin/service
Requires(preun): /sbin/chkconfig, /sbin/service

%description
The cachefilesd daemon manages the caching files and directory that are
that are used by network filesystems such a AFS and NFS to
do persistent caching to the local disk.

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

install -m 644 cachefilesd.conf %{buildroot}%{_sysconfdir}
install -m 755 cachefilesd.initd %{buildroot}%{_sysconfdir}/rc.d/init.d/cachefilesd

%clean
rm -rf $RPM_BUILD_ROOT

%post
/sbin/chkconfig --add %{name}

%preun
if [ $1 -eq 0 ]; then
	/sbin/service cachefilesd stop
	/sbin/chkconfig --del %{name}
fi

%postun
if [ "$1" -ge 1 ]; then
	/sbin/service cachefilesd condrestart > /dev/null
fi


%files
%defattr(-,root,root)
%doc README
%config(noreplace) %{_sysconfdir}/cachefilesd.conf
%attr(0755,root,root) %{_sysconfdir}/rc.d/init.d/cachefilesd
/sbin/*
%{_mandir}/*/*

%changelog
* Wed Aug 30 2006 David Howells <dhowells@redhat.com> 0.6-1
- Mark __error() as attribute format printf
- Fix up format errors shown up

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
