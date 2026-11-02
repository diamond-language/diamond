# Packages a tree packaging/build.sh has already built and staged; it compiles
# nothing itself. Build with packaging/build.sh rpm, which defines
# diamond_version and diamond_stage.
%global debug_package %{nil}

Name:           d6-lang
Version:        %{diamond_version}
Release:        1%{?dist}
Summary:        The d6 programming language
License:        MIT
URL:            https://getdiamond.codes

# The package was called diamond-lang until the rename; this one replaces it.
Provides:       diamond-lang = %{version}-%{release}
Obsoletes:      diamond-lang <= %{version}-%{release}

# facet downloads cuts with curl; git is only needed for git-source cuts.
Requires:       curl
Requires:       ca-certificates
Recommends:     git
# diamond build compiles and links a standalone executable with a C compiler
# against these libraries' development files.
Recommends:     gcc
Recommends:     sqlite-devel
Recommends:     libpq-devel
Recommends:     mariadb-connector-c-devel
Recommends:     openssl-devel
Recommends:     libxcrypt-devel
Recommends:     zlib-devel

%description
d6 is a gradually typed, object-oriented language with a register-based
bytecode VM. This package provides the d6 interpreter and REPL, the facet
package manager, the d6-lsp language server, the d6-dap debug adapter,
and the runtime kit d6 build links standalone executables against. The former diamond,
diamond-lsp and diamond-dap commands remain as links to them.

%install
cp -a %{diamond_stage}/. %{buildroot}/

%files
%{_bindir}/d6
%{_bindir}/diamond
%{_bindir}/facet
%{_bindir}/d6-lsp
%{_bindir}/diamond-lsp
%{_bindir}/d6-dap
%{_bindir}/diamond-dap
/usr/lib/diamond
%license %{_datadir}/licenses/d6-lang/LICENSE
%doc %{_docdir}/d6-lang/README.md
