#!/bin/bash
# build_rpm.sh — Build an installable RPM for pg_table_guard
# Run as a regular user with sudo access on AlmaLinux 8 / RHEL 8.
# Usage: bash build_rpm.sh [--pg-version 17]

set -euo pipefail

PG_VERSION=17

while [[ $# -gt 0 ]]; do
    case $1 in
        --pg-version) PG_VERSION="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SNAME="pg_table_guard"
VERSION=$(grep "^Version:" pg_table_guard.spec | awk '{print $2}')
TARBALL="${SNAME}-${VERSION}.tar.gz"

info()  { echo "[INFO]  $*"; }
ok()    { echo "[OK]    $*"; }
err()   { echo "[ERROR] $*" >&2; exit 1; }

# ── 1. Install build tools ────────────────────────────────────────────────────
info "Installing build tools..."
sudo dnf install -y rpm-build rpmdevtools gcc make redhat-rpm-config \
    postgresql${PG_VERSION}-devel 2>/dev/null || true
ok "Build tools ready"

# ── 2. Set up rpmbuild directory tree ────────────────────────────────────────
info "Setting up rpmbuild environment..."
rpmdev-setuptree
ok "rpmbuild tree ready at ~/rpmbuild/"

# ── 3. Create source tarball ─────────────────────────────────────────────────
info "Creating source tarball ${TARBALL}..."
SRCDIR=$(pwd)
TMPDIR=$(mktemp -d)
mkdir -p "${TMPDIR}/${SNAME}-${VERSION}"

# Copy source files (exclude git, spec, build scripts, and build artifacts)
cp table_guard.c \
   table_guard.control \
   "table_guard--1.0.sql" \
   Makefile \
   "${TMPDIR}/${SNAME}-${VERSION}/"

tar -czf ~/rpmbuild/SOURCES/${TARBALL} -C "$TMPDIR" "${SNAME}-${VERSION}"
rm -rf "$TMPDIR"
ok "Tarball created at ~/rpmbuild/SOURCES/${TARBALL}"

# ── 4. Copy spec file ─────────────────────────────────────────────────────────
info "Copying spec file..."
# Update PG version in spec if different from default
sed "s/^%global pgmajorversion.*/%global pgmajorversion ${PG_VERSION}/" \
    "${SRCDIR}/pg_table_guard.spec" > ~/rpmbuild/SPECS/pg_table_guard_${PG_VERSION}.spec
ok "Spec file ready"

# ── 5. Build the RPM ─────────────────────────────────────────────────────────
info "Building RPM..."
rpmbuild -ba ~/rpmbuild/SPECS/pg_table_guard_${PG_VERSION}.spec
ok "RPM built successfully"

# ── 6. Show output ────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════"
echo " RPM packages created:"
find ~/rpmbuild/RPMS -name "${SNAME}_${PG_VERSION}*.rpm" | while read f; do
    echo "   $f"
done
echo ""
echo " To install on this machine:"
echo "   sudo dnf install ~/rpmbuild/RPMS/x86_64/${SNAME}_${PG_VERSION}-${VERSION}-1.*.rpm"
echo ""
echo " To install on other machines, copy the RPM and run:"
echo "   sudo dnf install ./${SNAME}_${PG_VERSION}-${VERSION}-1.*.rpm"
echo "════════════════════════════════════════════════════════"
