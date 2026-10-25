# Packages a tree packaging/build.sh has already built and staged; it compiles
# nothing itself. Build with packaging/build.sh rpm, which defines
# diamond_version and diamond_stage.
%global debug_package %{nil}

Name:           diamond-lang
Version:        %{diamond_version}
Release:        1%{?dist}
Summary:        The Diamond programming language
License:        MIT
URL:            https://getdiamond.codes

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
Diamond is a gradually typed, object-oriented language with a register-based
bytecode VM. This package provides the diamond interpreter and REPL, the facet
package manager, the diamond-lsp language server, the diamond-dap debug adapter,
and the runtime kit diamond build links standalone executables against.

%install
cp -a %{diamond_stage}/. %{buildroot}/

%files
%{_bindir}/diamond
%{_bindir}/facet
%{_bindir}/diamond-lsp
%{_bindir}/diamond-dap
/usr/lib/diamond
%license %{_datadir}/licenses/diamond-lang/LICENSE
%doc %{_docdir}/diamond-lang/README.md
