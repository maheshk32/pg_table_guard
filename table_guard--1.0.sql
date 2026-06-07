-- table_guard--1.0.sql
--
-- Extension install script.
-- The actual protection logic lives in table_guard.so, loaded via
-- shared_preload_libraries.  This file creates helper objects so
-- DBAs can inspect and manage protection settings from inside psql.

-- Version marker
CREATE FUNCTION table_guard_version()
RETURNS text
LANGUAGE sql IMMUTABLE
AS $$ SELECT '1.0'::text $$;

-- ----------------------------------------------------------------
-- View: current protection settings for this database session
-- ----------------------------------------------------------------
CREATE VIEW table_guard_settings AS
SELECT
    current_setting('table_guard.scope',             true) AS scope,
    current_setting('table_guard.protected_tables',  true) AS protected_tables,
    current_database()                                      AS database;

COMMENT ON VIEW table_guard_settings IS
'Shows the active table_guard GUC settings for the current session.';

-- ----------------------------------------------------------------
-- Usage reminder function
-- ----------------------------------------------------------------
CREATE FUNCTION table_guard_help()
RETURNS text
LANGUAGE sql IMMUTABLE
AS $$
SELECT $help$
table_guard — scope configuration
──────────────────────────────────────────────────────────────────

SCOPE OPTIONS
  global    Protect all tables in ALL databases (default).
            Set in postgresql.conf:
              table_guard.scope = 'global'

  database  Protect all tables only in databases where
            CREATE EXTENSION table_guard has been run.
            Set per database:
              ALTER DATABASE mydb SET table_guard.scope = 'database';

  table     Protect only specific listed tables or schemas.
            Entry formats (comma-separated):
              database.schema.table  -- exact table in exact database
              database.schema.*      -- all tables in schema in exact database
              (entries without a database prefix are ignored)

            Examples:
              -- Single table in one DB:
              ALTER DATABASE mydb SET table_guard.protected_tables =
                'mydb.public.orders';

              -- Entire schema in one DB:
              ALTER DATABASE mydb SET table_guard.protected_tables =
                'mydb.public.*';

              -- Mixed, in postgresql.conf across multiple DBs:
              table_guard.protected_tables =
                'db1.public.*, db2.audit.*, db1.public.orders'

CHECK CURRENT SETTINGS
  SELECT * FROM table_guard_settings;

APPLY IMMEDIATELY (current session only — not persisted):
  SET table_guard.scope = 'table';
  SET table_guard.protected_tables = 'public.orders';

RELOAD AFTER ALTER DATABASE (no restart needed):
  SELECT pg_reload_conf();   -- picks up ALTER DATABASE changes for new connections
$help$::text
$$;

COMMENT ON FUNCTION table_guard_help() IS
'Prints configuration reference for table_guard scope settings.';
