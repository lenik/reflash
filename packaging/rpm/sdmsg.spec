# Version is injected by packaging/rpm/Makefile via `zfr version`.
# RPM Version cannot contain '-'; use `zfr version -r` (hyphens → '_').
# srcversion is the unsanitized Meson/git version and names the tarball.
%{!?version:%global version 0.0.0}
%{!?srcversion:%global srcversion %{version}}

Name:           sdmsg
Version:        %{version}
Release:        1%{?dist}
Summary:        Refresh flash storage by rewriting device data in place

License:        AGPL-3.0-or-later
URL:            https://github.com/lenik/sdmsg
Packager:       Lenik <sdmsg@bodz.net>
Source0:        %{name}-%{srcversion}.tar.xz

BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconf
BuildRequires:  asciidoctor
BuildRequires:  sqlite-devel
BuildRequires:  openssl-devel
BuildRequires:  wxGTK3-devel

%description
sdmsg ("SD massage") rewrites block devices or regular files in place to
fight bit-rot on flash media that sits unused. Supports linear raw rewrite
and recursive FAT/exFAT/NTFS/ext walks with SQLite tracking and an optional
wxWidgets progress UI.

%prep
%setup -q -n %{name}-%{srcversion}

%build
meson setup build \
    --prefix=%{_prefix} \
    --bindir=%{_bindir} \
    --datadir=%{_datadir} \
    --mandir=%{_mandir} \
    --sysconfdir=%{_sysconfdir} \
    --localstatedir=%{_localstatedir} \
    --buildtype=plain
meson compile -C build

%install
meson install -C build --destdir=%{buildroot}

%files
%{_bindir}/sdmsg
%{_datadir}/bash-completion/completions/sdmsg
%{_mandir}/man1/sdmsg.1*
%{_datadir}/doc/%{name}/

%changelog
* Thu Aug 20 2026 Lenik <sdmsg@bodz.net>
- Align spec with debian/control (Meson, AGPL-3.0-or-later).
- Version comes from `zfr version`, the same method meson.build uses.
