/*
 * table_guard.c
 *
 * PostgreSQL extension that prevents:
 *   1. DROP TABLE
 *   2. TRUNCATE TABLE
 *   3. DELETE FROM <table> with no WHERE clause
 *
 * Protection scope is controlled via GUC parameters:
 *
 *   table_guard.scope = global    -- all databases, all tables (default)
 *   table_guard.scope = database  -- only DBs where CREATE EXTENSION was run
 *   table_guard.scope = table     -- only tables listed in protected_tables
 *
 *   table_guard.protected_tables = 'public.orders, public.customers'
 *       (comma-separated schema.table list; used when scope = table)
 *
 * Scope can be set globally in postgresql.conf, or per-database:
 *
 *   ALTER DATABASE mydb SET table_guard.scope = 'database';
 *   ALTER DATABASE mydb SET table_guard.scope = 'table';
 *   ALTER DATABASE mydb SET table_guard.protected_tables = 'public.orders';
 *
 * Hooks used:
 *   - ProcessUtility_hook  : intercepts DDL (DROP TABLE, TRUNCATE)
 *   - ExecutorRun_hook     : intercepts DML (DELETE without WHERE)
 */

#include "postgres.h"

#include "fmgr.h"
#include "tcop/utility.h"
#include "executor/executor.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"
#include "catalog/namespace.h"
#include "commands/extension.h"
#include "catalog/pg_database.h"
#include "access/htup_details.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "storage/lock.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

/* ----------------------------------------------------------------
 * GUC: scope enum
 * ---------------------------------------------------------------- */

typedef enum TGScope
{
    TG_SCOPE_GLOBAL   = 0,  /* protect all tables in all databases (default) */
    TG_SCOPE_DATABASE = 1,  /* protect all tables in databases where extension is installed */
    TG_SCOPE_TABLE    = 2   /* protect only specific tables */
} TGScope;

static int   tg_scope = TG_SCOPE_GLOBAL;
static char *tg_protected_tables = NULL;

static const struct config_enum_entry tg_scope_options[] = {
    {"global",   TG_SCOPE_GLOBAL,   false},
    {"database", TG_SCOPE_DATABASE, false},
    {"table",    TG_SCOPE_TABLE,    false},
    {NULL, 0, false}
};

/* ----------------------------------------------------------------
 * Hook chain pointers — saved so we can call the previous hook
 * ---------------------------------------------------------------- */

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static ExecutorRun_hook_type    prev_ExecutorRun    = NULL;

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */

void _PG_init(void);
void _PG_fini(void);

/*
 * ProcessUtility hook signature changed in PG 14:
 *   PG 14+: added bool readOnlyTree parameter
 *   PG 13:  no readOnlyTree parameter
 */
#if PG_VERSION_NUM >= 140000
static void guard_ProcessUtility(PlannedStmt *pstmt,
                                  const char *queryString,
                                  bool readOnlyTree,
                                  ProcessUtilityContext context,
                                  ParamListInfo params,
                                  QueryEnvironment *queryEnv,
                                  DestReceiver *dest,
                                  QueryCompletion *qc);
#else
static void guard_ProcessUtility(PlannedStmt *pstmt,
                                  const char *queryString,
                                  ProcessUtilityContext context,
                                  ParamListInfo params,
                                  QueryEnvironment *queryEnv,
                                  DestReceiver *dest,
                                  QueryCompletion *qc);
#endif

/*
 * ExecutorRun hook signature changed in PG 18:
 *   PG 18+: removed execute_once parameter
 *   PG 14-17: has execute_once parameter
 */
#if PG_VERSION_NUM >= 180000
static void guard_ExecutorRun(QueryDesc *queryDesc,
                               ScanDirection direction,
                               uint64 count);
#else
static void guard_ExecutorRun(QueryDesc *queryDesc,
                               ScanDirection direction,
                               uint64 count,
                               bool execute_once);
#endif

/* ----------------------------------------------------------------
 * Helper: get current database name via syscache (no missing header needed)
 * ---------------------------------------------------------------- */

static const char *
tg_current_dbname(void)
{
    HeapTuple   tup;
    const char *name;

    tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
    if (!HeapTupleIsValid(tup))
        return NULL;
    name = pstrdup(NameStr(((Form_pg_database) GETSTRUCT(tup))->datname));
    ReleaseSysCache(tup);
    return name;
}

/* ----------------------------------------------------------------
 * Helper: is table_guard installed in the current database?
 * Used to implement scope = database.
 * ---------------------------------------------------------------- */

static bool
tg_extension_installed(void)
{
    return OidIsValid(get_extension_oid("table_guard", true /* missing_ok */));
}

/* ----------------------------------------------------------------
 * Helper: should protection be active in the current database?
 * ---------------------------------------------------------------- */

static bool
tg_db_active(void)
{
    switch ((TGScope) tg_scope)
    {
        case TG_SCOPE_GLOBAL:
            /* Always active regardless of extension installation */
            return true;

        case TG_SCOPE_DATABASE:
        case TG_SCOPE_TABLE:
            /* Active only if CREATE EXTENSION table_guard was run in this DB */
            return tg_extension_installed();
    }
    return false;
}

/* ----------------------------------------------------------------
 * Helper: is the entry in the GUC protected list?
 * Only called when scope = table.
 *
 * Supported entry formats (comma-separated):
 *   database.schema.table  -- exact table in exact database
 *   database.schema.*      -- all tables in schema in exact database
 *
 * Database must always be specified to avoid unintended cross-database protection.
 *
 * Examples:
 *   'db1.public.orders'               -- single table in db1
 *   'db1.public.*'                    -- all tables in public schema in db1
 *   'db1.public.orders, db2.audit.*'  -- mixed
 * ---------------------------------------------------------------- */

static bool
tg_name_in_list(const char *dbname, const char *schemaname, const char *tablename)
{
    char *list_copy;
    char *token;
    char *saveptr;
    bool  found = false;

    if (!tg_protected_tables || tg_protected_tables[0] == '\0')
        return false;

    list_copy = pstrdup(tg_protected_tables);

    for (token = strtok_r(list_copy, ",", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ",", &saveptr))
    {
        char *start = token;
        char *end;
        char *dot1;
        char *dot2;

        /* Trim leading whitespace */
        while (*start == ' ' || *start == '\t') start++;
        /* Trim trailing whitespace */
        end = start + strlen(start) - 1;
        while (end > start && (*end == ' ' || *end == '\t'))
            *end-- = '\0';

        dot1 = strchr(start, '.');
        dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;

        if (dot1 && dot2)
        {
            /*
             * Three-part: database.schema.table  or  database.schema.*
             * '*' as table part means all tables in that schema.
             */
            *dot1 = '\0';
            *dot2 = '\0';
            if (strcmp(start,    dbname)     == 0 &&
                strcmp(dot1 + 1, schemaname) == 0 &&
                (strcmp(dot2 + 1, "*") == 0 ||
                 strcmp(dot2 + 1, tablename) == 0))
            {
                found = true;
                break;
            }
        }
        /* Entries without a database qualifier are ignored */
    }

    pfree(list_copy);
    return found;
}

/* ----------------------------------------------------------------
 * Helper: is a relation (by OID) in the protected list?
 * When scope is not table, all tables are protected.
 * ---------------------------------------------------------------- */

static bool
tg_relid_protected(Oid relid)
{
    const char *relname;
    const char *nspname;

    /* global/database scope: every table is protected */
    if ((TGScope) tg_scope != TG_SCOPE_TABLE)
        return true;

    if (!OidIsValid(relid))
        return false;

    relname = get_rel_name(relid);
    if (!relname)
        return false;

    nspname = get_namespace_name(get_rel_namespace(relid));
    if (!nspname)
        return false;

    return tg_name_in_list(tg_current_dbname(), nspname, relname);
}

/* ----------------------------------------------------------------
 * Helper: is a RangeVar (table name from SQL) protected?
 * Resolves to OID using search_path so unqualified names work.
 * ---------------------------------------------------------------- */

static bool
tg_rangevar_protected(RangeVar *rv)
{
    Oid relid;

    if ((TGScope) tg_scope != TG_SCOPE_TABLE)
        return true;

    /*
     * Resolve name → OID via the current search_path.
     * missing_ok=true: if the table doesn't exist we return false and let
     * the standard handler produce the "table not found" error.
     */
    relid = RangeVarGetRelid(rv, NoLock, true /* missing_ok */);
    if (!OidIsValid(relid))
        return false;

    return tg_relid_protected(relid);
}

/* ----------------------------------------------------------------
 * _PG_init — called once at server start when the library is loaded
 * ---------------------------------------------------------------- */

void
_PG_init(void)
{
    /* Register table_guard.scope GUC */
    DefineCustomEnumVariable(
        "table_guard.scope",
        "Protection scope: global = all databases and all tables (default); "
        "database = only databases where CREATE EXTENSION table_guard was run; "
        "table = only tables listed in table_guard.protected_tables.",
        NULL,
        &tg_scope,
        TG_SCOPE_GLOBAL,
        tg_scope_options,
        PGC_SUSET,          /* requires superuser; settable per-DB with ALTER DATABASE */
        0,
        NULL, NULL, NULL
    );

    /* Register table_guard.protected_tables GUC */
    DefineCustomStringVariable(
        "table_guard.protected_tables",
        "Comma-separated list of tables to protect when scope = table. "
        "Format: schema.table  —  example: 'public.orders, public.customers'",
        NULL,
        &tg_protected_tables,
        "",                 /* default: empty (no tables) */
        PGC_SUSET,
        0,
        NULL, NULL, NULL
    );

    /* Reserve the GUC prefix so unrecognised table_guard.* params are warned */
#if PG_VERSION_NUM >= 150000
    MarkGUCPrefixReserved("table_guard");
#else
    EmitWarningsOnPlaceholders("table_guard");
#endif

    /* Install hooks, saving any previously registered hooks so we can chain */
    prev_ProcessUtility = ProcessUtility_hook;
    ProcessUtility_hook = guard_ProcessUtility;

    prev_ExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = guard_ExecutorRun;
}

/* ----------------------------------------------------------------
 * _PG_fini — restore original hooks when library is unloaded
 * ---------------------------------------------------------------- */

void
_PG_fini(void)
{
    ProcessUtility_hook = prev_ProcessUtility;
    ExecutorRun_hook    = prev_ExecutorRun;
}

/* ----------------------------------------------------------------
 * guard_ProcessUtility — intercepts DDL statements
 *
 * DROP TABLE objects list: each element is a List of name strings
 * (e.g. ["public", "orders"]), resolved via makeRangeVarFromNameList.
 *
 * TRUNCATE relations list: each element is a RangeVar directly.
 * ---------------------------------------------------------------- */

#if PG_VERSION_NUM >= 140000
static void
guard_ProcessUtility(PlannedStmt *pstmt,
                     const char *queryString,
                     bool readOnlyTree,
                     ProcessUtilityContext context,
                     ParamListInfo params,
                     QueryEnvironment *queryEnv,
                     DestReceiver *dest,
                     QueryCompletion *qc)
#else
static void
guard_ProcessUtility(PlannedStmt *pstmt,
                     const char *queryString,
                     ProcessUtilityContext context,
                     ParamListInfo params,
                     QueryEnvironment *queryEnv,
                     DestReceiver *dest,
                     QueryCompletion *qc)
#endif
{
    Node *parsetree = pstmt->utilityStmt;

    if (tg_db_active())
    {
        /* ----- Block DROP TABLE ----- */
        if (IsA(parsetree, DropStmt))
        {
            DropStmt *stmt = (DropStmt *) parsetree;

            if (stmt->removeType == OBJECT_TABLE)
            {
                ListCell *cell;
                foreach(cell, stmt->objects)
                {
                    /*
                     * Each element in DropStmt->objects for OBJECT_TABLE is a
                     * List of name strings (schema-qualified). Convert to RangeVar.
                     */
                    RangeVar *rv = makeRangeVarFromNameList(castNode(List, lfirst(cell)));

                    if (tg_rangevar_protected(rv))
                        ereport(ERROR,
                                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                                 errmsg("table_guard: DROP TABLE is disabled on \"%s\"",
                                        rv->relname),
                                 errhint("Contact your DBA to remove this restriction.")));
                }
            }
        }

        /* ----- Block DROP SCHEMA ----- */
        if (IsA(parsetree, DropStmt))
        {
            DropStmt *stmt = (DropStmt *) parsetree;

            if (stmt->removeType == OBJECT_SCHEMA)
            {
                ListCell *cell;
                foreach(cell, stmt->objects)
                {
                    /*
                     * For OBJECT_SCHEMA, each element in objects is a plain
                     * string Value node containing the schema name.
                     */
                    char *schemaname = strVal(lfirst(cell));

                    ereport(ERROR,
                            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                             errmsg("table_guard: DROP SCHEMA is disabled on \"%s\"",
                                    schemaname),
                             errhint("Contact your DBA to remove this restriction.")));
                }
            }
        }

        /* ----- Block DROP DATABASE ----- */
        if (IsA(parsetree, DropdbStmt))
        {
            DropdbStmt *stmt = (DropdbStmt *) parsetree;

            ereport(ERROR,
                    (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                     errmsg("table_guard: DROP DATABASE is disabled on \"%s\"",
                            stmt->dbname),
                     errhint("Contact your DBA to remove this restriction.")));
        }

        /* ----- Block TRUNCATE ----- */
        if (IsA(parsetree, TruncateStmt))
        {
            TruncateStmt *stmt = (TruncateStmt *) parsetree;
            ListCell     *cell;

            foreach(cell, stmt->relations)
            {
                /* TruncateStmt->relations contains RangeVar nodes directly */
                RangeVar *rv = lfirst_node(RangeVar, cell);

                if (tg_rangevar_protected(rv))
                    ereport(ERROR,
                            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                             errmsg("table_guard: TRUNCATE is disabled on \"%s\"",
                                    rv->relname),
                             errhint("Contact your DBA to remove this restriction.")));
            }
        }
    }

    /* Pass to next hook or standard handler */
#if PG_VERSION_NUM >= 140000
    if (prev_ProcessUtility)
        prev_ProcessUtility(pstmt, queryString, readOnlyTree,
                            context, params, queryEnv, dest, qc);
    else
        standard_ProcessUtility(pstmt, queryString, readOnlyTree,
                                context, params, queryEnv, dest, qc);
#else
    if (prev_ProcessUtility)
        prev_ProcessUtility(pstmt, queryString,
                            context, params, queryEnv, dest, qc);
    else
        standard_ProcessUtility(pstmt, queryString,
                                context, params, queryEnv, dest, qc);
#endif
}

/* ----------------------------------------------------------------
 * guard_ExecutorRun — intercepts DML, blocks whole-table DELETE
 *
 * A DELETE with no WHERE clause produces a plan whose top node has
 * an empty qual list (qual == NIL).  If there is a WHERE clause the
 * planner populates qual with the filter conditions.
 * ---------------------------------------------------------------- */

#if PG_VERSION_NUM >= 180000
static void
guard_ExecutorRun(QueryDesc *queryDesc,
                  ScanDirection direction,
                  uint64 count)
#else
static void
guard_ExecutorRun(QueryDesc *queryDesc,
                  ScanDirection direction,
                  uint64 count,
                  bool execute_once)
#endif
{
    if (tg_db_active() &&
        queryDesc->plannedstmt->commandType == CMD_DELETE)
    {
        PlannedStmt *pstmt   = queryDesc->plannedstmt;
        Plan        *topPlan = pstmt->planTree;

        if (topPlan != NULL && topPlan->qual == NIL)
        {
            bool block = false;

            if ((TGScope) tg_scope != TG_SCOPE_TABLE)
            {
                /* global or database scope — block all whole-table DELETEs */
                block = true;
            }
            else if (pstmt->resultRelations != NIL)
            {
                /*
                 * table scope — check if the target table is in the protected list.
                 * resultRelations is a list of RT indexes (1-based) into rtable.
                 */
                int            rtindex = linitial_int(pstmt->resultRelations);
                RangeTblEntry *rte     = (RangeTblEntry *)
                                            list_nth(pstmt->rtable, rtindex - 1);
                block = tg_relid_protected(rte->relid);
            }

            if (block)
                ereport(ERROR,
                        (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                         errmsg("table_guard: DELETE without WHERE clause is disabled"),
                         errhint("Add a WHERE clause, or contact your DBA.")));
        }
    }

#if PG_VERSION_NUM >= 180000
    if (prev_ExecutorRun)
        prev_ExecutorRun(queryDesc, direction, count);
    else
        standard_ExecutorRun(queryDesc, direction, count);
#else
    if (prev_ExecutorRun)
        prev_ExecutorRun(queryDesc, direction, count, execute_once);
    else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
}
