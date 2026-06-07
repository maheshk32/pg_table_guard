#!/bin/bash
# install.sh — Build and install pg_table_guard on AlmaLinux 8 / RHEL 8
# Run as a user with sudo access.
# Usage: bash install.sh [--pg-version 17] [--uninstall]

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────
PG_VERSION=17
UNINSTALL=0
PG_CONFIG=""

# ── Argument parsing ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --pg-version) PG_VERSION="$2"; shift 2 ;;
        --uninstall)  UNINSTALL=1;     shift   ;;
        *) echo "Unknown option: $1"; exit 1   ;;
    esac
done

PG_CONFIG="/usr/pgsql-${PG_VERSION}/bin/pg_config"
PG_CTL="postgresql-${PG_VERSION}"
PG_DATA="/var/lib/pgsql/${PG_VERSION}/data/postgresql.conf"

# ── Helpers ──────────────────────────────────────────────────────────────────
info()  { echo "[INFO]  $*"; }
ok()    { echo "[OK]    $*"; }
err()   { echo "[ERROR] $*" >&2; exit 1; }

check_root() {
    if [[ $EUID -eq 0 ]]; then
        err "Do not run as root. Run as a sudo-enabled user."
    fi
}

# ── Uninstall ────────────────────────────────────────────────────────────────
uninstall() {
    info "Uninstalling pg_table_guard..."

    info "Removing shared_preload_libraries entry from postgresql.conf..."
    sudo sed -i "s/,\s*table_guard//g; s/table_guard,\s*//g; s/^shared_preload_libraries\s*=\s*'table_guard'/# shared_preload_libraries = ''/g" "$PG_DATA"

    info "Removing installed files..."
    PKGLIBDIR=$("$PG_CONFIG" --pkglibdir)
    SHAREDIR=$("$PG_CONFIG" --sharedir)
    sudo rm -f "${PKGLIBDIR}/table_guard.so"
    sudo rm -f "${PKGLIBDIR}/bitcode/table_guard/table_guard.bc"
    sudo rm -f "${PKGLIBDIR}/table_guard.index.bc"
    sudo rm -f "${SHAREDIR}/extension/table_guard.control"
    sudo rm -f "${SHAREDIR}/extension/table_guard--1.0.sql"

    info "Restarting PostgreSQL ${PG_VERSION}..."
    sudo systemctl restart "$PG_CTL"

    ok "pg_table_guard uninstalled. Remember to run DROP EXTENSION table_guard; in any databases that had it."
    exit 0
}

# ── Main install ─────────────────────────────────────────────────────────────
check_root

[[ $UNINSTALL -eq 1 ]] && uninstall

info "Starting pg_table_guard installation for PostgreSQL ${PG_VERSION}..."

# 1. Check pg_config exists
if [[ ! -x "$PG_CONFIG" ]]; then
    err "pg_config not found at $PG_CONFIG. Is postgresql${PG_VERSION}-devel installed?"
fi
ok "Found pg_config at $PG_CONFIG"

# 2. Install build dependencies if missing
info "Checking build dependencies..."
MISSING=()
for pkg in gcc make; do
    command -v "$pkg" &>/dev/null || MISSING+=("$pkg")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    info "Installing missing packages: ${MISSING[*]}"
    sudo dnf install -y "${MISSING[@]}" redhat-rpm-config
fi
ok "Build dependencies satisfied"

# 3. Build
info "Building extension..."
make clean PG_CONFIG="$PG_CONFIG" 2>/dev/null || true
make PG_CONFIG="$PG_CONFIG"
ok "Build successful"

# 4. Install
info "Installing extension files..."
sudo make install PG_CONFIG="$PG_CONFIG"
ok "Files installed"

# 5. Configure shared_preload_libraries
info "Configuring postgresql.conf..."
if grep -q "shared_preload_libraries" "$PG_DATA"; then
    # Entry exists — check if table_guard is already in it
    if grep -q "table_guard" "$PG_DATA"; then
        ok "table_guard already in shared_preload_libraries — skipping"
    else
        # Append to existing value
        sudo sed -i "s/^\(shared_preload_libraries\s*=\s*'\)/\1table_guard, /" "$PG_DATA"
        ok "Appended table_guard to shared_preload_libraries"
    fi
else
    # No entry — add a new one
    echo "shared_preload_libraries = 'table_guard'" | sudo tee -a "$PG_DATA" > /dev/null
    ok "Added shared_preload_libraries = 'table_guard'"
fi

# 6. Restart PostgreSQL
info "Restarting PostgreSQL ${PG_VERSION}..."
sudo systemctl restart "$PG_CTL"
sleep 2

# 7. Verify it loaded
info "Verifying extension loaded..."
if sudo journalctl -u "$PG_CTL" -n 20 --no-pager | grep -q "table_guard.*error\|error.*table_guard"; then
    err "PostgreSQL log shows an error loading table_guard. Check: sudo journalctl -u $PG_CTL -n 50"
fi
ok "PostgreSQL restarted cleanly"

# ── Done ─────────────────────────────────────────────────────────────────────
cat <<EOF

[DONE] pg_table_guard installed successfully.

Default scope: global (all databases and tables are protected).

To enable in a specific database:
  sudo -u postgres psql -d <your_database> -c "CREATE EXTENSION table_guard;"

To change scope (run as superuser in psql):
  -- Protect only databases where extension is installed:
  ALTER DATABASE mydb SET table_guard.scope = 'database';

  -- Protect specific tables only:
  ALTER DATABASE mydb SET table_guard.scope = 'table';
  ALTER DATABASE mydb SET table_guard.protected_tables = 'mydb.public.orders, mydb.public.customers';

  -- Protect an entire schema in one database:
  ALTER DATABASE mydb SET table_guard.protected_tables = 'mydb.public.*';

  SELECT pg_reload_conf();  -- apply without restart

To uninstall:
  bash install.sh --uninstall

EOF
