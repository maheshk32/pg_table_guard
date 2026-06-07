override with_llvm = no

MODULES   = table_guard
EXTENSION = table_guard
DATA      = table_guard--1.0.sql
PG_CONFIG ?= pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
