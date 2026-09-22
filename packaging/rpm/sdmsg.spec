# Version is injected by packaging/rpm/Makefile via `zfr version`.
# RPM Version cannot contain '-'; use `zfr version -r` (hyphens → '_').
# srcversion is the unsanitized Meson/git version and names the tarball.
%{!?version:%global version 0.0.0}
%{!?srcversion:%global srcversion %{version}}

Name:           sdmsg
Version:        %{version}
Release:        1%{?dist}
Summary:        Simple C CLI project template with example app

License:        AGPL-3.0-or-later
URL:            https://github.com/lenik/sdmsg
Packager:       Lenik <sdmsg@bodz.net>
Source0:        %{name}-%{srcversion}.tar.xz

BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconf
BuildRequires:  asciidoctor

%description
sdmsg is a Meson-based template for small C command-line utilities
(no shared/static library packaging). It ships the sdmsg example
application, AsciiDoc man pages, bash completion, and Debian packaging.

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
