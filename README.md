# pg_table_guard

A PostgreSQL extension that prevents accidental data loss by blocking:

- `DROP TABLE`
- `TRUNCATE`
- `DELETE` without a `WHERE` clause

Protection is configurable — guard all databases globally, a single database, or specific tables only.

---

## Requirements

- AlmaLinux 8 / RHEL 8 (or compatible)
- PostgreSQL 14, 15, 16, 17, or 18

---

## Installation

### 1. Install the RPM

Pick the command for your PostgreSQL version:

**PostgreSQL 14:**
```bash
sudo dnf install https://github.com/maheshk32/pg_table_guard/releases/latest/download/pg_table_guard_14-1.0-1.el8.x86_64.rpm
```

**PostgreSQL 15:**
```bash
sudo dnf install https://github.com/maheshk32/pg_table_guard/releases/latest/download/pg_table_guard_15-1.0-1.el8.x86_64.rpm
```

**PostgreSQL 16:**
```bash
sudo dnf install https://github.com/maheshk32/pg_table_guard/releases/latest/download/pg_table_guard_16-1.0-1.el8.x86_64.rpm
```

**PostgreSQL 17:**
```bash
sudo dnf install https://github.com/maheshk32/pg_table_guard/releases/latest/download/pg_table_guard_17-1.0-1.el8.x86_64.rpm
```

**PostgreSQL 18:**
```bash
sudo dnf install https://github.com/maheshk32/pg_table_guard/releases/latest/download/pg_table_guard_18-1.0-1.el8.x86_64.rpm
```

Or download the RPM first, then install locally:

```bash
sudo dnf install ./pg_table_guard_17-1.0-1.el8.x86_64.rpm
```

### 2. Enable the extension

Add `table_guard` to `shared_preload_libraries` in `postgresql.conf`:

```ini
shared_preload_libraries = 'table_guard'
```

> **Note:** `postgresql.conf` is typically at `/var/lib/pgsql/17/data/postgresql.conf`

### 3. Restart PostgreSQL

```bash
sudo systemctl restart postgresql-17
```

### 4. (Optional) Install per database

To use the helper views and functions, run in each database you want to track:

```sql
CREATE EXTENSION table_guard;
```

---

## Configuration

All settings are GUC parameters. Set them in `postgresql.conf` (server-wide) or per database with `ALTER DATABASE`.

### `table_guard.scope`

Controls which operations are protected.

| Value      | Behaviour |
|------------|-----------|
| `global`   | **(Default)** Block DROP/TRUNCATE/DELETE on all tables in all databases |
| `database` | Block only in databases where `CREATE EXTENSION table_guard` has been run |
| `table`    | Block only the tables listed in `table_guard.protected_tables` |

### `table_guard.protected_tables`

Comma-separated list of tables to protect (only used when `scope = 'table'`).

Each entry must be in one of these formats:

| Format | Meaning |
|--------|---------|
| `db.schema.table` | Protect a specific table |
| `db.schema.*` | Protect all tables in a schema |

**Examples:**

```sql
-- Protect a single table
ALTER DATABASE mydb SET table_guard.protected_tables = 'mydb.public.orders';

-- Protect all tables in a schema
ALTER DATABASE mydb SET table_guard.protected_tables = 'mydb.audit.*';

-- Multiple entries
ALTER DATABASE mydb SET table_guard.protected_tables = 'mydb.public.orders, mydb.audit.*';

-- Apply changes without restart
SELECT pg_reload_conf();
```

---

## Examples

### Protect all databases (default)

Nothing to configure — protection is active as soon as `shared_preload_libraries` is set and PostgreSQL is restarted.

### Protect only one database

```sql
-- Run as superuser in the target database
ALTER DATABASE mydb SET table_guard.scope = 'database';
SELECT pg_reload_conf();
```

### Protect specific tables only

```sql
ALTER DATABASE mydb SET table_guard.scope = 'table';
ALTER DATABASE mydb SET table_guard.protected_tables = 'mydb.public.orders, mydb.public.customers';
SELECT pg_reload_conf();
```

---

## Verify it works

```sql
-- This should be blocked:
DROP TABLE orders;
-- ERROR: table_guard: DROP TABLE is not allowed

-- This should be blocked:
DELETE FROM orders;
-- ERROR: table_guard: DELETE without WHERE clause is not allowed

-- This is allowed:
DELETE FROM orders WHERE id = 42;
```

---

## Check current settings

If the extension is installed in a database:

```sql
SELECT * FROM table_guard_settings;
```

```sql
SELECT table_guard_help();
```

---

## Uninstall

```bash
# Replace 17 with your PostgreSQL version (16, 17, or 18)
sudo dnf remove pg_table_guard_17
```

Then remove `table_guard` from `shared_preload_libraries` in `postgresql.conf` and restart PostgreSQL.

---

## Building from source

See [build_rpm.sh](build_rpm.sh) for local RPM builds, or push a `v*` tag to trigger the GitHub Actions release workflow.
