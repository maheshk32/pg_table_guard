# Build & Install Guide — `table_guard` PostgreSQL Extension
**Platform:** AlmaLinux 8.9  
**PostgreSQL version:** 17 (17.x)  
**Purpose:** Blocks `DROP TABLE`, `TRUNCATE`, and `DELETE` without a `WHERE` clause at the server level.

---

## 1. Prerequisites

### 1.1 Install the PGDG repository

PostgreSQL 17 is not available in the AlmaLinux 8 AppStream module, so use the official PGDG repo:

```bash
sudo dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-8-x86_64/pgdg-redhat-repo-latest.noarch.rpm

# Disable the built-in stream to avoid conflicts
sudo dnf module disable postgresql -y
```

### 1.2 Install PostgreSQL 17 server + development headers

```bash
# Server (if not already installed)
sudo dnf install -y postgresql17-server

# Development package — provides pg_config, server headers, PGXS Makefile
sudo dnf install -y postgresql17-devel
```

### 1.3 Install build tools

```bash
sudo dnf install -y gcc make redhat-rpm-config
```

### 1.4 Ensure the build user has sudo access

The `make install` step writes to `/usr/pgsql-17/` and requires root. If your build user (e.g. `pgs5dba`) is not already a sudoer, add them to the `wheel` group:

```bash
# Run as root or an existing sudo user
sudo usermod -aG wheel pgs5dba
```

Set a password for the user if one isn't set:

```bash
sudo passwd pgs5dba
```

> **Note:** Log out and back in for the group change to take effect.

### 1.5 Initialize the database cluster (if fresh install)

```bash
sudo /usr/pgsql-17/bin/postgresql-17-setup initdb
sudo systemctl enable postgresql-17
sudo systemctl start postgresql-17
```

---

## 2. Get the Source Code

Transfer the four files below to a working directory on your AlmaLinux machine (e.g. `~/table_guard/`):

```
table_guard.c
table_guard.control
table_guard--1.0.sql
Makefile
```

Using `scp` from another machine:

```bash
scp table_guard.c table_guard.control table_guard--1.0.sql Makefile \
    user@alma-host:~/table_guard/
```

Or create the directory and paste the files directly on the server:

```bash
mkdir ~/table_guard && cd ~/table_guard
```

---

## 3. Build the Extension

`pg_config` for PG 17 lives at `/usr/pgsql-17/bin/pg_config` (not on PATH by default):

```bash
cd ~/table_guard
make PG_CONFIG=/usr/pgsql-17/bin/pg_config
```

Expected output ends with something like:
```
gcc ... -shared -o table_guard.so table_guard.o
```

---

## 4. Install into PostgreSQL

```bash
sudo make install PG_CONFIG=/usr/pgsql-17/bin/pg_config
```

Verify the files landed in the right places:

```bash
ls $(/usr/pgsql-17/bin/pg_config --pkglibdir)/table_guard.so
ls $(/usr/pgsql-17/bin/pg_config --sharedir)/extension/table_guard*
```

---

## 5. Configure PostgreSQL to Load the Library

The extension uses server hooks, so the shared library **must** be preloaded at server start.

Edit `postgresql.conf`:

```bash
sudo vi /var/lib/pgsql/17/data/postgresql.conf
```

Add or update the `shared_preload_libraries` line:

```ini
shared_preload_libraries = 'table_guard'
```

If other libraries are already listed, append with a comma:

```ini
shared_preload_libraries = 'pg_stat_statements, table_guard'
```

---

## 6. Restart PostgreSQL

```bash
sudo systemctl restart postgresql-17
sudo systemctl status  postgresql-17
```

Check the server log to confirm the library loaded without errors:

```bash
sudo journalctl -u postgresql-17 -n 30
```

You should see no errors mentioning `table_guard`.

---

## 7. Enable the Extension in Your Database

Connect as a superuser and run once per database you want to protect:

```bash
sudo -u postgres psql -d your_database_name
```

```sql
CREATE EXTENSION table_guard;
```

Verify:

```sql
SELECT * FROM pg_extension WHERE extname = 'table_guard';
```

---

## 8. Configure Protection Scope

The default scope is `global` — all tables in all databases are protected. You can change this without restarting PostgreSQL.

### Option A — Global (default, all databases)

In `postgresql.conf` (or leave unset):

```ini
table_guard.scope = 'global'
```

### Option B — Per database (only databases where extension is installed)

```sql
ALTER DATABASE mydb SET table_guard.scope = 'database';
-- Then run CREATE EXTENSION table_guard; in mydb to activate protection
-- Databases without the extension are unprotected
```

### Option C — Per table (only specific tables)

```sql
ALTER DATABASE mydb SET table_guard.scope = 'table';
ALTER DATABASE mydb SET table_guard.protected_tables = 'public.orders, public.customers';
```

Reload config for new connections (no restart needed):

```sql
SELECT pg_reload_conf();
```

Apply immediately for the current session only:

```sql
SET table_guard.scope = 'table';
SET table_guard.protected_tables = 'public.orders';
```

Check current settings:

```sql
SELECT * FROM table_guard_settings;
SELECT table_guard_help();
```

---

## 10. Test the Protection

```sql
-- Should raise: table_guard: DROP TABLE is disabled
DROP TABLE some_table;

-- Should raise: table_guard: TRUNCATE is disabled
TRUNCATE some_table;

-- Should raise: table_guard: DELETE without WHERE clause is disabled
DELETE FROM some_table;

-- Should succeed (has a WHERE clause)
DELETE FROM some_table WHERE id = 999;
```

---

## 11. Remove the Extension

To disable protection on a specific database:

```sql
DROP EXTENSION table_guard;
```

To fully unload the library, also remove it from `shared_preload_libraries` in `postgresql.conf` and restart PostgreSQL.

---

## 12. Rebuild After a PostgreSQL Minor Update

After a minor-version upgrade (e.g. 17.4 → 17.10) rebuild and reinstall the `.so`:

```bash
cd ~/table_guard
make clean
make PG_CONFIG=/usr/pgsql-17/bin/pg_config
sudo make install PG_CONFIG=/usr/pgsql-17/bin/pg_config
sudo systemctl restart postgresql-17
```

---

## File Reference

| File | Purpose |
|---|---|
| `table_guard.c` | C source — hook implementations |
| `table_guard.control` | Extension metadata |
| `table_guard--1.0.sql` | SQL run by `CREATE EXTENSION` |
| `Makefile` | PGXS build rules |

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `pg_config: command not found` | Use full path: `PG_CONFIG=/usr/pgsql-17/bin/pg_config` |
| `ERROR: could not open extension control file` | Run `sudo make install PG_CONFIG=...`; check `pg_config --sharedir` |
| Hooks not firing after `CREATE EXTENSION` | Ensure `shared_preload_libraries` includes `table_guard` and PostgreSQL-17 was restarted |
| `DELETE` without WHERE still works | Load `table_guard` last in `shared_preload_libraries` so its `ExecutorRun_hook` takes precedence |
| `dnf install postgresql17-devel` not found | Confirm PGDG repo is enabled: `dnf repolist | grep pgdg` |
| `llvm-lto: No such file or directory` during `make install` | PGXS tries to build LLVM bitcode for JIT. Either install `llvm` (`sudo dnf install llvm`) or disable it by adding `override with_llvm = no` as the first line of the `Makefile` — bitcode is not needed for this extension. |
| `sudo: command not found` or permission denied on `make install` | Add the build user to the wheel group: `sudo usermod -aG wheel <username>`, then log out and back in. |
