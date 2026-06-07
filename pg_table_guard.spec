%global pgmajorversion 17
%global pginstdir      /usr/pgsql-%{pgmajorversion}
%global sname          pg_table_guard

Name:           %{sname}_%{pgmajorversion}
Version:        1.0
Release:        1%{?dist}
Summary:        PostgreSQL %{pgmajorversion} extension to prevent DROP TABLE, TRUNCATE, and whole-table DELETE
License:        PostgreSQL
URL:            https://github.com/yourorg/pg_table_guard

Source0:        %{sname}-%{version}.tar.gz

BuildRequires:  postgresql%{pgmajorversion}-devel
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  redhat-rpm-config

Requires:       postgresql%{pgmajorversion}-server

%description
pg_table_guard is a PostgreSQL extension that prevents accidental data loss by
blocking DROP TABLE, TRUNCATE, and DELETE without a WHERE clause.

Protection scope is configurable via GUC parameters — protect all databases
globally, only specific databases, or only specific tables and schemas.

%prep
%setup -q -n %{sname}-%{version}

%build
make PG_CONFIG=%{pginstdir}/bin/pg_config

%install
make install \
    PG_CONFIG=%{pginstdir}/bin/pg_config \
    DESTDIR=%{buildroot}

%files
%{pginstdir}/lib/table_guard.so
%{pginstdir}/share/extension/table_guard.control
%{pginstdir}/share/extension/table_guard--1.0.sql

%post
echo ""
echo "pg_table_guard %{version} installed for PostgreSQL %{pgmajorversion}."
echo ""
echo "To activate, add table_guard to shared_preload_libraries in postgresql.conf:"
echo "  shared_preload_libraries = 'table_guard'"
echo ""
echo "Then restart PostgreSQL:"
echo "  systemctl restart postgresql-%{pgmajorversion}"
echo ""
echo "Optionally, run in each database you want to track:"
echo "  psql -d <dbname> -c \"CREATE EXTENSION table_guard;\""
echo ""
echo "Scope configuration (default is global — all DBs protected):"
echo "  ALTER DATABASE mydb SET table_guard.scope = 'database';"
echo "  ALTER DATABASE mydb SET table_guard.scope = 'table';"
echo "  ALTER DATABASE mydb SET table_guard.protected_tables = 'mydb.public.*';"
echo ""

%preun
if [ $1 -eq 0 ]; then
    # Full uninstall (not upgrade) — remind user to clean up
    echo ""
    echo "pg_table_guard removed."
    echo "Remember to remove 'table_guard' from shared_preload_libraries"
    echo "in postgresql.conf and restart PostgreSQL."
    echo ""
fi

%changelog
* %(date "+%a %b %d %Y") Mahesh K <k.mahesh1991@gmail.com> - 1.0-1
- Initial release
