CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -Werror -std=c99 -D_POSIX_C_SOURCE=200809L -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -O2 -flto

# Detect operating system
UNAME_S := $(shell uname -s)

# Set LDFLAGS based on OS
ifeq ($(UNAME_S),Linux)
    # Check if STATIC=1 is set (for Alpine static builds)
    ifdef STATIC
        CFLAGS += -static
        PKG_CONFIG_CURL := $(shell pkg-config --libs --static libcurl 2>/dev/null)
        ifneq ($(PKG_CONFIG_CURL),)
            LDFLAGS = -static $(PKG_CONFIG_CURL)
        else
            LDFLAGS = -static -lcurl
        endif
    else
        # On Linux, use dynamic linking by default
        # Static linking is complex due to Kerberos dependencies in libcurl
        # For static builds, use Alpine Linux with STATIC=1 or build curl without GSSAPI
        PKG_CONFIG_CURL := $(shell pkg-config --libs --static libcurl 2>/dev/null)
        ifneq ($(PKG_CONFIG_CURL),)
            LDFLAGS = $(PKG_CONFIG_CURL)
        else
            # Fallback to basic curl linking
            LDFLAGS = -lcurl
        endif
    endif
else ifeq ($(UNAME_S),Darwin)
    # On macOS, use dynamic linking
    LDFLAGS = -lcurl
else
    # Default fallback
    LDFLAGS = -lcurl
endif

# Directories
SRCDIR = src
BUILDDIR = build
BINDIR = bin

# Rulr (Mini Datalog) library
RULR_SRCDIR = $(SRCDIR)/rulr
RULR_LIB = $(BINDIR)/librulr.a
RULR_SOURCES = $(RULR_SRCDIR)/rulr.c \
               $(RULR_SRCDIR)/rulr_dl.c \
               $(RULR_SRCDIR)/builtin_rules.c \
               $(RULR_SRCDIR)/host_helpers.c \
               $(RULR_SRCDIR)/frontend/lexer.c \
               $(RULR_SRCDIR)/frontend/parser.c \
               $(RULR_SRCDIR)/frontend/ast_serialize.c \
               $(RULR_SRCDIR)/ir/ir_builder.c \
               $(RULR_SRCDIR)/runtime/runtime.c \
               $(RULR_SRCDIR)/engine/engine.c
RULR_OBJECTS = $(patsubst $(SRCDIR)/rulr/%.c,$(BUILDDIR)/rulr/%.o,$(RULR_SOURCES))
RULR_DRIVER = $(BINDIR)/rulr-demo
RULR_DRIVER_SRC = $(RULR_SRCDIR)/driver_main.c
RULR_DRIVER_OBJ = $(BUILDDIR)/rulr/driver_main.o
RULR_CFLAGS = $(CFLAGS) -Isrc -Isrc/rulr

# Built-in rules
BUILTIN_RULES_DIR = rulr/rules
BUILTIN_RULES_ZIP = $(BUILDDIR)/builtin_rules.zip
BUILTIN_RULES_DLC = $(BUILDDIR)/builtin_rules

# Rulr compiler (rulrc)
RULRC = $(BINDIR)/rulrc
RULRC_SRC = $(RULR_SRCDIR)/rulrc_main.c
RULRC_OBJ = $(BUILDDIR)/rulr/rulrc_main.o

# Files
TARGET_FILE = elm-wrap
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/alloc.c \
          $(SRCDIR)/log.c \
          $(SRCDIR)/progname.c \
          $(SRCDIR)/commands/wrappers/init.c \
          $(SRCDIR)/commands/wrappers/make.c \
          $(SRCDIR)/commands/wrappers/repl.c \
          $(SRCDIR)/commands/wrappers/reactor.c \
          $(SRCDIR)/commands/wrappers/bump.c \
          $(SRCDIR)/commands/wrappers/diff.c \
          $(SRCDIR)/commands/wrappers/publish.c \
          $(SRCDIR)/commands/wrappers/live.c \
          $(SRCDIR)/config.c \
          $(SRCDIR)/commands/publish/docs/docs.c \
          $(SRCDIR)/commands/publish/docs/elm_docs.c \
          $(SRCDIR)/commands/publish/docs/type_maps.c \
          $(SRCDIR)/commands/publish/docs/tree_util.c \
          $(SRCDIR)/commands/publish/docs/comment_extract.c \
          $(SRCDIR)/commands/publish/docs/type_qualify.c \
          $(SRCDIR)/commands/publish/docs/module_parse.c \
          $(SRCDIR)/commands/publish/docs/decl_extract.c \
          $(SRCDIR)/commands/publish/docs/docs_json.c \
          $(SRCDIR)/commands/publish/docs/dependency_cache.c \
          $(SRCDIR)/commands/publish/docs/path_util.c \
          $(SRCDIR)/commands/publish/package/package_publish.c \
          $(SRCDIR)/commands/code/code.c \
          $(SRCDIR)/commands/code/format.c \
          $(SRCDIR)/commands/policy/policy.c \
          $(SRCDIR)/commands/review/review.c \
          $(SRCDIR)/commands/review/reporter.c \
          $(SRCDIR)/commands/package/package_common.c \
          $(SRCDIR)/commands/package/install_cmd.c \
          $(SRCDIR)/commands/package/cache_cmd.c \
          $(SRCDIR)/commands/package/remove_cmd.c \
          $(SRCDIR)/commands/package/info_cmd.c \
          $(SRCDIR)/commands/package/upgrade_cmd.c \
          $(SRCDIR)/commands/debug/debug.c \
          $(SRCDIR)/commands/debug/include_tree.c \
          $(SRCDIR)/commands/repository/repository.c \
          $(SRCDIR)/import_tree.c \
          $(SRCDIR)/commands/cache/check/cache_check.c \
          $(SRCDIR)/commands/cache/full_scan/cache_full_scan.c \
          $(SRCDIR)/commands/publish/docs/vendor/tree-sitter/lib.c \
          $(SRCDIR)/commands/publish/docs/vendor/tree-sitter-elm/parser.c \
          $(SRCDIR)/commands/publish/docs/vendor/tree-sitter-elm/scanner.c \
          $(SRCDIR)/install_check.c \
          $(SRCDIR)/elm_json.c \
          $(SRCDIR)/elm_compiler.c \
          $(SRCDIR)/cache.c \
          $(SRCDIR)/solver.c \
          $(SRCDIR)/pgsolver/solver_common.c \
          $(SRCDIR)/cJSON.c \
          $(SRCDIR)/http_client.c \
          $(SRCDIR)/registry.c \
          $(SRCDIR)/protocol_v1/package_fetch.c \
          $(SRCDIR)/protocol_v1/install.c \
          $(SRCDIR)/protocol_v1/solver/solver.c \
          $(SRCDIR)/protocol_v2/index_fetch.c \
          $(SRCDIR)/protocol_v2/install.c \
          $(SRCDIR)/protocol_v2/solver/v2_registry.c \
          $(SRCDIR)/protocol_v2/solver/pg_elm_v2.c \
          $(SRCDIR)/protocol_v2/solver/solver.c \
          $(SRCDIR)/install_env.c \
          $(SRCDIR)/fileutil.c \
          $(SRCDIR)/commands/wrappers/elm_cmd_common.c \
          $(SRCDIR)/elm_project.c \
          $(SRCDIR)/env_defaults.c \
          $(SRCDIR)/global_context.c \
          $(SRCDIR)/pgsolver/pg_core.c \
          $(SRCDIR)/pgsolver/pg_elm.c \
          $(SRCDIR)/ast/skeleton.c \
          $(SRCDIR)/ast/qualify.c \
          $(SRCDIR)/ast/canonicalize.c \
          $(SRCDIR)/ast/util.c \
          $(SRCDIR)/vendor/sha1.c \
          $(SRCDIR)/vendor/sha256.c \
          $(SRCDIR)/vendor/miniz.c
BUILDINFO_SRC = $(BUILDDIR)/buildinfo.c
OBJECTS = $(BUILDDIR)/main.o \
          $(BUILDDIR)/alloc.o \
          $(BUILDDIR)/log.o \
          $(BUILDDIR)/progname.o \
          $(BUILDDIR)/init.o \
          $(BUILDDIR)/make.o \
          $(BUILDDIR)/repl.o \
          $(BUILDDIR)/reactor.o \
          $(BUILDDIR)/bump.o \
          $(BUILDDIR)/diff.o \
          $(BUILDDIR)/publish.o \
          $(BUILDDIR)/live.o \
          $(BUILDDIR)/config.o \
          $(BUILDDIR)/docs.o \
          $(BUILDDIR)/elm_docs.o \
          $(BUILDDIR)/type_maps.o \
          $(BUILDDIR)/tree_util.o \
          $(BUILDDIR)/comment_extract.o \
          $(BUILDDIR)/type_qualify.o \
          $(BUILDDIR)/module_parse.o \
          $(BUILDDIR)/decl_extract.o \
          $(BUILDDIR)/docs_json.o \
          $(BUILDDIR)/dependency_cache.o \
          $(BUILDDIR)/path_util.o \
          $(BUILDDIR)/package_publish.o \
          $(BUILDDIR)/code.o \
          $(BUILDDIR)/format.o \
          $(BUILDDIR)/policy.o \
          $(BUILDDIR)/review.o \
          $(BUILDDIR)/reporter.o \
          $(BUILDDIR)/package_common.o \
          $(BUILDDIR)/install_cmd.o \
          $(BUILDDIR)/cache_cmd.o \
          $(BUILDDIR)/remove_cmd.o \
          $(BUILDDIR)/info_cmd.o \
          $(BUILDDIR)/upgrade_cmd.o \
          $(BUILDDIR)/debug.o \
          $(BUILDDIR)/include_tree.o \
          $(BUILDDIR)/repository.o \
          $(BUILDDIR)/import_tree.o \
          $(BUILDDIR)/cache_check.o \
          $(BUILDDIR)/cache_full_scan.o \
          $(BUILDDIR)/ts_lib.o \
          $(BUILDDIR)/ts_elm_parser.o \
          $(BUILDDIR)/ts_elm_scanner.o \
          $(BUILDDIR)/install_check.o \
          $(BUILDDIR)/elm_json.o \
          $(BUILDDIR)/elm_compiler.o \
          $(BUILDDIR)/cache.o \
          $(BUILDDIR)/solver.o \
          $(BUILDDIR)/solver_common.o \
          $(BUILDDIR)/cJSON.o \
          $(BUILDDIR)/http_client.o \
          $(BUILDDIR)/registry.o \
          $(BUILDDIR)/package_fetch.o \
          $(BUILDDIR)/v1_install.o \
          $(BUILDDIR)/v1_solver.o \
          $(BUILDDIR)/index_fetch.o \
          $(BUILDDIR)/v2_install.o \
          $(BUILDDIR)/v2_registry.o \
          $(BUILDDIR)/pg_elm_v2.o \
          $(BUILDDIR)/v2_solver.o \
          $(BUILDDIR)/install_env.o \
          $(BUILDDIR)/fileutil.o \
          $(BUILDDIR)/elm_cmd_common.o \
          $(BUILDDIR)/elm_project.o \
          $(BUILDDIR)/env_defaults.o \
          $(BUILDDIR)/global_context.o \
          $(BUILDDIR)/pg_core.o \
          $(BUILDDIR)/pg_elm.o \
          $(BUILDDIR)/ast_skeleton.o \
          $(BUILDDIR)/ast_qualify.o \
          $(BUILDDIR)/ast_canonicalize.o \
          $(BUILDDIR)/ast_util.o \
          $(BUILDDIR)/sha1.o \
          $(BUILDDIR)/sha256.o \
          $(BUILDDIR)/miniz.o \
          $(BUILDDIR)/buildinfo.o
TARGET = $(BINDIR)/$(TARGET_FILE)
PG_CORE_TEST = $(BINDIR)/pg_core_test
PG_FILE_TEST = $(BINDIR)/pg_file_test
VERSION_FILE = VERSION
ENV_DEFAULTS_FILE = ENV_DEFAULTS

# Parse ENV_DEFAULTS file (format: KEY=VALUE, one per line)
# Default values if file doesn't exist
ENV_DEFAULT_REGISTRY_V2_FULL_INDEX_URL ?= 
ENV_DEFAULT_REPOSITORY_LOCAL_PATH ?= ~/.elm-wrap/repository

ifneq ($(wildcard $(ENV_DEFAULTS_FILE)),)
  # Read each line from ENV_DEFAULTS and set corresponding make variables
  ENV_DEFAULT_REGISTRY_V2_FULL_INDEX_URL := $(shell grep '^ELM_WRAP_REGISTRY_V2_FULL_INDEX_URL=' $(ENV_DEFAULTS_FILE) 2>/dev/null | cut -d= -f2-)
  ENV_DEFAULT_REPOSITORY_LOCAL_PATH := $(shell grep '^ELM_WRAP_REPOSITORY_LOCAL_PATH=' $(ENV_DEFAULTS_FILE) 2>/dev/null | cut -d= -f2-)
endif

# SBOM Configuration (override buildinfo.mk defaults)
SBOM_FILE = SBOM.spdx
SBOM_PACKAGE_NAME = elm-wrap
SBOM_SPDX_LICENSE = BSD-3-Clause
SBOM_SUPPLIER = Person: Damir Simunic (damir@oomm.dev)
SBOM_HOMEPAGE = https://github.com/dsimunic/elm-wrap

# Include buildinfo.mk for version generation
include buildinfo.mk

# Ensure 'all' is the default target (must be set after include)
.DEFAULT_GOAL := all

# Installation directories
PREFIX ?= /usr/local
BINDIR_INSTALL = $(PREFIX)/bin
USER_PREFIX = $(HOME)/.local
USER_BINDIR = $(USER_PREFIX)/bin

.PHONY: all clean pg_core_test pg_file_test test check dist distcheck install install-user uninstall uninstall-user rulrc compile-builtin-rules

.DEFAULT_GOAL := all

all: $(TARGET) append-builtin-rules

rulrc: $(RULRC)

# Compile all .dl files in rulr/rules to .dlc format
compile-builtin-rules: $(RULRC)
	@echo "Compiling built-in rules..."
	@mkdir -p $(BUILTIN_RULES_DLC)
	@for f in $(BUILTIN_RULES_DIR)/*.dl; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .dl); \
			$(RULRC) compile "$$f" -o $(BUILTIN_RULES_DLC)/"$$base".dlc; \
		fi; \
	done

# Create zip file from compiled rules and append to binary
append-builtin-rules: $(TARGET) compile-builtin-rules
	@echo "Creating built-in rules archive..."
	@rm -f $(BUILTIN_RULES_ZIP)
	@cd $(BUILTIN_RULES_DLC) && zip -q ../builtin_rules.zip *.dlc 2>/dev/null || true
	@if [ -f $(BUILTIN_RULES_ZIP) ]; then \
		echo "Appending rules to binary..."; \
		cat $(BUILTIN_RULES_ZIP) >> $(TARGET); \
	fi

# Generate buildinfo.c before compiling
$(BUILDINFO_SRC): buildinfo.mk $(VERSION_FILE) $(wildcard $(ENV_DEFAULTS_FILE))
	@$(MAKE) -f buildinfo.mk generate-buildinfo \
		BUILDDIR=$(BUILDDIR) \
		VERSION_FILE=$(VERSION_FILE) \
		CC=$(CC)
	@echo "/* Environment variable defaults (project-specific) */" >> $(BUILDDIR)/buildinfo.c
	@echo "const char *env_default_registry_v2_full_index_url = \"$(ENV_DEFAULT_REGISTRY_V2_FULL_INDEX_URL)\";" >> $(BUILDDIR)/buildinfo.c
	@echo "const char *env_default_repository_local_path = \"$(ENV_DEFAULT_REPOSITORY_LOCAL_PATH)\";" >> $(BUILDDIR)/buildinfo.c

# Build buildinfo object
$(BUILDDIR)/buildinfo.o: $(BUILDINFO_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

# Build main object
$(BUILDDIR)/main.o: $(SRCDIR)/main.c $(SRCDIR)/buildinfo.h $(SRCDIR)/install.h $(SRCDIR)/commands/wrappers/make.h $(SRCDIR)/commands/wrappers/init.h $(SRCDIR)/commands/wrappers/repl.h $(SRCDIR)/commands/wrappers/reactor.h $(SRCDIR)/commands/wrappers/bump.h $(SRCDIR)/commands/wrappers/diff.h $(SRCDIR)/commands/wrappers/publish.h $(SRCDIR)/commands/wrappers/live.h $(SRCDIR)/config.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build alloc object
$(BUILDDIR)/alloc.o: $(SRCDIR)/alloc.c $(SRCDIR)/alloc.h $(SRCDIR)/larena.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build log object
$(BUILDDIR)/log.o: $(SRCDIR)/log.c $(SRCDIR)/log.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build progname object
$(BUILDDIR)/progname.o: $(SRCDIR)/progname.c $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build init object
$(BUILDDIR)/init.o: $(SRCDIR)/commands/wrappers/init.c $(SRCDIR)/commands/wrappers/init.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build make object
$(BUILDDIR)/make.o: $(SRCDIR)/commands/wrappers/make.c $(SRCDIR)/commands/wrappers/make.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build repl object
$(BUILDDIR)/repl.o: $(SRCDIR)/commands/wrappers/repl.c $(SRCDIR)/commands/wrappers/repl.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build reactor object
$(BUILDDIR)/reactor.o: $(SRCDIR)/commands/wrappers/reactor.c $(SRCDIR)/commands/wrappers/reactor.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build bump object
$(BUILDDIR)/bump.o: $(SRCDIR)/commands/wrappers/bump.c $(SRCDIR)/commands/wrappers/bump.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build diff object
$(BUILDDIR)/diff.o: $(SRCDIR)/commands/wrappers/diff.c $(SRCDIR)/commands/wrappers/diff.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build publish object
$(BUILDDIR)/publish.o: $(SRCDIR)/commands/wrappers/publish.c $(SRCDIR)/commands/wrappers/publish.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build live object
$(BUILDDIR)/live.o: $(SRCDIR)/commands/wrappers/live.c $(SRCDIR)/commands/wrappers/live.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h $(SRCDIR)/elm_compiler.h $(SRCDIR)/global_context.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build config object
$(BUILDDIR)/config.o: $(SRCDIR)/config.c $(SRCDIR)/config.h $(SRCDIR)/cache.h $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build docs object
$(BUILDDIR)/docs.o: $(SRCDIR)/commands/publish/docs/docs.c $(SRCDIR)/commands/publish/docs/docs.h $(SRCDIR)/commands/publish/docs/elm_docs.h $(SRCDIR)/commands/publish/docs/docs_json.h $(SRCDIR)/commands/publish/docs/dependency_cache.h $(SRCDIR)/commands/publish/docs/path_util.h $(SRCDIR)/cache.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build elm_docs object
$(BUILDDIR)/elm_docs.o: $(SRCDIR)/commands/publish/docs/elm_docs.c $(SRCDIR)/commands/publish/docs/elm_docs.h $(SRCDIR)/commands/publish/docs/dependency_cache.h $(SRCDIR)/commands/publish/docs/type_maps.h $(SRCDIR)/commands/publish/docs/tree_util.h $(SRCDIR)/commands/publish/docs/comment_extract.h $(SRCDIR)/commands/publish/docs/type_qualify.h $(SRCDIR)/commands/publish/docs/module_parse.h $(SRCDIR)/commands/publish/docs/decl_extract.h $(SRCDIR)/commands/publish/docs/docs_json.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build type_maps object
$(BUILDDIR)/type_maps.o: $(SRCDIR)/commands/publish/docs/type_maps.c $(SRCDIR)/commands/publish/docs/type_maps.h $(SRCDIR)/commands/publish/docs/dependency_cache.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build tree_util object
$(BUILDDIR)/tree_util.o: $(SRCDIR)/commands/publish/docs/tree_util.c $(SRCDIR)/commands/publish/docs/tree_util.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build comment_extract object
$(BUILDDIR)/comment_extract.o: $(SRCDIR)/commands/publish/docs/comment_extract.c $(SRCDIR)/commands/publish/docs/comment_extract.h $(SRCDIR)/commands/publish/docs/tree_util.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build type_qualify object
$(BUILDDIR)/type_qualify.o: $(SRCDIR)/commands/publish/docs/type_qualify.c $(SRCDIR)/commands/publish/docs/type_qualify.h $(SRCDIR)/commands/publish/docs/type_maps.h $(SRCDIR)/commands/publish/docs/tree_util.h $(SRCDIR)/commands/publish/docs/dependency_cache.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build module_parse object
$(BUILDDIR)/module_parse.o: $(SRCDIR)/commands/publish/docs/module_parse.c $(SRCDIR)/commands/publish/docs/module_parse.h $(SRCDIR)/commands/publish/docs/type_maps.h $(SRCDIR)/commands/publish/docs/tree_util.h $(SRCDIR)/commands/publish/docs/dependency_cache.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build decl_extract object
$(BUILDDIR)/decl_extract.o: $(SRCDIR)/commands/publish/docs/decl_extract.c $(SRCDIR)/commands/publish/docs/decl_extract.h $(SRCDIR)/commands/publish/docs/type_maps.h $(SRCDIR)/commands/publish/docs/tree_util.h $(SRCDIR)/commands/publish/docs/type_qualify.h $(SRCDIR)/commands/publish/docs/comment_extract.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build docs_json object
$(BUILDDIR)/docs_json.o: $(SRCDIR)/commands/publish/docs/docs_json.c $(SRCDIR)/commands/publish/docs/docs_json.h $(SRCDIR)/commands/publish/docs/elm_docs.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build dependency_cache object
$(BUILDDIR)/dependency_cache.o: $(SRCDIR)/commands/publish/docs/dependency_cache.c $(SRCDIR)/commands/publish/docs/dependency_cache.h $(SRCDIR)/commands/publish/docs/path_util.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build path_util object
$(BUILDDIR)/path_util.o: $(SRCDIR)/commands/publish/docs/path_util.c $(SRCDIR)/commands/publish/docs/path_util.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build package_publish command object
$(BUILDDIR)/package_publish.o: $(SRCDIR)/commands/publish/package/package_publish.c $(SRCDIR)/commands/publish/package/package_publish.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h $(SRCDIR)/elm_json.h $(SRCDIR)/ast/skeleton.h $(SRCDIR)/dyn_array.h $(SRCDIR)/cJSON.h $(SRCDIR)/rulr/rulr.h $(SRCDIR)/rulr/rulr_dl.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -I$(SRCDIR)/rulr -c $< -o $@

# Build code command object
$(BUILDDIR)/code.o: $(SRCDIR)/commands/code/code.c $(SRCDIR)/commands/code/code.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build format command object
$(BUILDDIR)/format.o: $(SRCDIR)/commands/code/format.c $(SRCDIR)/commands/code/code.h $(SRCDIR)/commands/publish/docs/tree_util.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build review command object
$(BUILDDIR)/review.o: $(SRCDIR)/commands/review/review.c $(SRCDIR)/commands/review/review.h $(SRCDIR)/commands/review/reporter.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h $(SRCDIR)/elm_json.h $(SRCDIR)/ast/skeleton.h $(SRCDIR)/rulr/rulr.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -I$(SRCDIR)/rulr -c $< -o $@

# Build reporter object for review command
$(BUILDDIR)/reporter.o: $(SRCDIR)/commands/review/reporter.c $(SRCDIR)/commands/review/reporter.h $(SRCDIR)/alloc.h $(SRCDIR)/rulr/rulr.h $(SRCDIR)/rulr/runtime/runtime.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/rulr -c $< -o $@

# Build debug command object
$(BUILDDIR)/debug.o: $(SRCDIR)/commands/debug/debug.c $(SRCDIR)/commands/debug/debug.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build policy command object
$(BUILDDIR)/policy.o: $(SRCDIR)/commands/policy/policy.c $(SRCDIR)/commands/policy/policy.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h $(SRCDIR)/rulr/rulr_dl.h $(SRCDIR)/rulr/frontend/ast.h $(SRCDIR)/rulr/frontend/ast_serialize.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/rulr -c $< -o $@

# Build include_tree command object
$(BUILDDIR)/include_tree.o: $(SRCDIR)/commands/debug/include_tree.c $(SRCDIR)/commands/debug/debug.h $(SRCDIR)/alloc.h $(SRCDIR)/progname.h $(SRCDIR)/log.h $(SRCDIR)/dyn_array.h $(SRCDIR)/cJSON.h $(SRCDIR)/ast/skeleton.h $(SRCDIR)/import_tree.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build import_tree shared library object
$(BUILDDIR)/import_tree.o: $(SRCDIR)/import_tree.c $(SRCDIR)/import_tree.h $(SRCDIR)/alloc.h $(SRCDIR)/dyn_array.h $(SRCDIR)/cJSON.h $(SRCDIR)/ast/skeleton.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build cache_check object
$(BUILDDIR)/cache_check.o: $(SRCDIR)/commands/cache/check/cache_check.c $(SRCDIR)/commands/cache/check/cache_check.h $(SRCDIR)/cache.h $(SRCDIR)/registry.h $(SRCDIR)/install_env.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/progname.h $(SRCDIR)/fileutil.h $(SRCDIR)/import_tree.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build cache_full_scan object
$(BUILDDIR)/cache_full_scan.o: $(SRCDIR)/commands/cache/full_scan/cache_full_scan.c $(SRCDIR)/commands/cache/full_scan/cache_full_scan.h $(SRCDIR)/cache.h $(SRCDIR)/registry.h $(SRCDIR)/install_env.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/progname.h $(SRCDIR)/fileutil.h $(SRCDIR)/import_tree.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@
	$(CC) $(CFLAGS) -c $< -o $@

# Build tree-sitter lib object
$(BUILDDIR)/ts_lib.o: $(SRCDIR)/commands/publish/docs/vendor/tree-sitter/lib.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build tree-sitter-elm parser object
$(BUILDDIR)/ts_elm_parser.o: $(SRCDIR)/commands/publish/docs/vendor/tree-sitter-elm/parser.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build tree-sitter-elm scanner object
$(BUILDDIR)/ts_elm_scanner.o: $(SRCDIR)/commands/publish/docs/vendor/tree-sitter-elm/scanner.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build package command objects
$(BUILDDIR)/package_common.o: $(SRCDIR)/commands/package/package_common.c $(SRCDIR)/commands/package/package_common.h $(SRCDIR)/alloc.h $(SRCDIR)/cache.h $(SRCDIR)/fileutil.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/install_cmd.o: $(SRCDIR)/commands/package/install_cmd.c $(SRCDIR)/commands/package/package_common.h $(SRCDIR)/install.h $(SRCDIR)/elm_json.h $(SRCDIR)/install_env.h $(SRCDIR)/registry.h $(SRCDIR)/cache.h $(SRCDIR)/solver.h $(SRCDIR)/http_client.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/progname.h $(SRCDIR)/fileutil.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/cache_cmd.o: $(SRCDIR)/commands/package/cache_cmd.c $(SRCDIR)/commands/package/package_common.h $(SRCDIR)/install.h $(SRCDIR)/elm_json.h $(SRCDIR)/install_env.h $(SRCDIR)/registry.h $(SRCDIR)/cache.h $(SRCDIR)/http_client.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/progname.h $(SRCDIR)/fileutil.h $(SRCDIR)/commands/cache/check/cache_check.h $(SRCDIR)/commands/cache/full_scan/cache_full_scan.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/remove_cmd.o: $(SRCDIR)/commands/package/remove_cmd.c $(SRCDIR)/commands/package/package_common.h $(SRCDIR)/install.h $(SRCDIR)/elm_json.h $(SRCDIR)/install_env.h $(SRCDIR)/solver.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/info_cmd.o: $(SRCDIR)/commands/package/info_cmd.c $(SRCDIR)/commands/package/package_common.h $(SRCDIR)/install.h $(SRCDIR)/install_check.h $(SRCDIR)/elm_json.h $(SRCDIR)/install_env.h $(SRCDIR)/registry.h $(SRCDIR)/protocol_v1/install.h $(SRCDIR)/protocol_v2/install.h $(SRCDIR)/protocol_v2/solver/v2_registry.h $(SRCDIR)/global_context.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/upgrade_cmd.o: $(SRCDIR)/commands/package/upgrade_cmd.c $(SRCDIR)/commands/package/package_common.h $(SRCDIR)/install.h $(SRCDIR)/elm_json.h $(SRCDIR)/install_env.h $(SRCDIR)/registry.h $(SRCDIR)/protocol_v1/install.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/progname.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build install_check object
$(BUILDDIR)/install_check.o: $(SRCDIR)/install_check.c $(SRCDIR)/install_check.h $(SRCDIR)/elm_json.h $(SRCDIR)/cJSON.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build elm_json object
$(BUILDDIR)/elm_json.o: $(SRCDIR)/elm_json.c $(SRCDIR)/elm_json.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build elm_compiler object
$(BUILDDIR)/elm_compiler.o: $(SRCDIR)/elm_compiler.c $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build cache object
$(BUILDDIR)/cache.o: $(SRCDIR)/cache.c $(SRCDIR)/cache.h $(SRCDIR)/elm_compiler.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build solver object
$(BUILDDIR)/solver.o: $(SRCDIR)/solver.c $(SRCDIR)/solver.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build solver_common object
$(BUILDDIR)/solver_common.o: $(SRCDIR)/pgsolver/solver_common.c $(SRCDIR)/pgsolver/solver_common.h $(SRCDIR)/solver.h $(SRCDIR)/elm_json.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build PubGrub core object
$(BUILDDIR)/pg_core.o: $(SRCDIR)/pgsolver/pg_core.c $(SRCDIR)/pgsolver/pg_core.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build Elm adapter object
$(BUILDDIR)/pg_elm.o: $(SRCDIR)/pgsolver/pg_elm.c $(SRCDIR)/pgsolver/pg_elm.h $(SRCDIR)/pgsolver/pg_core.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build AST skeleton object
$(BUILDDIR)/ast_skeleton.o: $(SRCDIR)/ast/skeleton.c $(SRCDIR)/ast/skeleton.h $(SRCDIR)/ast/util.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build AST qualify object
$(BUILDDIR)/ast_qualify.o: $(SRCDIR)/ast/qualify.c $(SRCDIR)/ast/qualify.h $(SRCDIR)/ast/skeleton.h $(SRCDIR)/ast/util.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build AST canonicalize object
$(BUILDDIR)/ast_canonicalize.o: $(SRCDIR)/ast/canonicalize.c $(SRCDIR)/ast/canonicalize.h $(SRCDIR)/ast/util.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build AST util object
$(BUILDDIR)/ast_util.o: $(SRCDIR)/ast/util.c $(SRCDIR)/ast/util.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR)/commands/publish/docs/vendor/tree-sitter -c $< -o $@

# Build pg_core test object
$(BUILDDIR)/pg_core_test.o: test/src/pg_core_test.c $(SRCDIR)/pgsolver/pg_core.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build pg_file test object
$(BUILDDIR)/pg_file_test.o: test/src/pg_file_test.c $(SRCDIR)/pgsolver/pg_core.h test/src/vendor/jsmn/jsmn.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build cJSON object
$(BUILDDIR)/cJSON.o: $(SRCDIR)/cJSON.c $(SRCDIR)/cJSON.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build http_client object
$(BUILDDIR)/http_client.o: $(SRCDIR)/http_client.c $(SRCDIR)/http_client.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build registry object
$(BUILDDIR)/registry.o: $(SRCDIR)/registry.c $(SRCDIR)/registry.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build package_fetch object
$(BUILDDIR)/package_fetch.o: $(SRCDIR)/protocol_v1/package_fetch.c $(SRCDIR)/protocol_v1/package_fetch.h $(SRCDIR)/install_env.h $(SRCDIR)/cache.h $(SRCDIR)/http_client.h $(SRCDIR)/cJSON.h $(SRCDIR)/vendor/sha1.h $(SRCDIR)/log.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build v1_install object (protocol v1 install functions)
$(BUILDDIR)/v1_install.o: $(SRCDIR)/protocol_v1/install.c $(SRCDIR)/protocol_v1/install.h $(SRCDIR)/install_env.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/log.h $(SRCDIR)/alloc.h $(SRCDIR)/fileutil.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build v1_solver object (protocol v1 solver functions)
$(BUILDDIR)/v1_solver.o: $(SRCDIR)/protocol_v1/solver/solver.c $(SRCDIR)/protocol_v1/solver/solver.h $(SRCDIR)/solver.h $(SRCDIR)/elm_json.h $(SRCDIR)/pgsolver/solver_common.h $(SRCDIR)/pgsolver/pg_elm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build index_fetch object (protocol v2)
$(BUILDDIR)/index_fetch.o: $(SRCDIR)/protocol_v2/index_fetch.c $(SRCDIR)/protocol_v2/index_fetch.h $(SRCDIR)/env_defaults.h $(SRCDIR)/http_client.h $(SRCDIR)/log.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build v2_install object (protocol v2 install functions)
$(BUILDDIR)/v2_install.o: $(SRCDIR)/protocol_v2/install.c $(SRCDIR)/protocol_v2/install.h $(SRCDIR)/protocol_v2/solver/v2_registry.h $(SRCDIR)/elm_json.h $(SRCDIR)/global_context.h $(SRCDIR)/log.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build v2_registry object (protocol v2 solver)
$(BUILDDIR)/v2_registry.o: $(SRCDIR)/protocol_v2/solver/v2_registry.c $(SRCDIR)/protocol_v2/solver/v2_registry.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h $(SRCDIR)/vendor/miniz.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build pg_elm_v2 object (protocol v2 solver)
$(BUILDDIR)/pg_elm_v2.o: $(SRCDIR)/protocol_v2/solver/pg_elm_v2.c $(SRCDIR)/protocol_v2/solver/pg_elm_v2.h $(SRCDIR)/protocol_v2/solver/v2_registry.h $(SRCDIR)/pgsolver/pg_core.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build v2_solver object (protocol v2 solver functions)
$(BUILDDIR)/v2_solver.o: $(SRCDIR)/protocol_v2/solver/solver.c $(SRCDIR)/protocol_v2/solver/solver.h $(SRCDIR)/solver.h $(SRCDIR)/elm_json.h $(SRCDIR)/pgsolver/solver_common.h $(SRCDIR)/protocol_v2/solver/pg_elm_v2.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build install_env object
$(BUILDDIR)/install_env.o: $(SRCDIR)/install_env.c $(SRCDIR)/install_env.h $(SRCDIR)/cache.h $(SRCDIR)/registry.h $(SRCDIR)/http_client.h $(SRCDIR)/cJSON.h $(SRCDIR)/vendor/sha1.h $(SRCDIR)/fileutil.h $(SRCDIR)/protocol_v1/package_fetch.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build fileutil object
$(BUILDDIR)/fileutil.o: $(SRCDIR)/fileutil.c $(SRCDIR)/fileutil.h $(SRCDIR)/vendor/miniz.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build elm_cmd_common object
$(BUILDDIR)/elm_cmd_common.o: $(SRCDIR)/commands/wrappers/elm_cmd_common.c $(SRCDIR)/commands/wrappers/elm_cmd_common.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/registry.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build elm_project object
$(BUILDDIR)/elm_project.o: $(SRCDIR)/elm_project.c $(SRCDIR)/elm_project.h $(SRCDIR)/fileutil.h $(SRCDIR)/cJSON.h $(SRCDIR)/dyn_array.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build env_defaults object
$(BUILDDIR)/env_defaults.o: $(SRCDIR)/env_defaults.c $(SRCDIR)/env_defaults.h $(SRCDIR)/buildinfo.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build global_context object
$(BUILDDIR)/global_context.o: $(SRCDIR)/global_context.c $(SRCDIR)/global_context.h $(SRCDIR)/env_defaults.h $(SRCDIR)/elm_compiler.h $(SRCDIR)/alloc.h $(SRCDIR)/log.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build repository object
$(BUILDDIR)/repository.o: $(SRCDIR)/commands/repository/repository.c $(SRCDIR)/commands/repository/repository.h $(SRCDIR)/env_defaults.h $(SRCDIR)/elm_compiler.h $(SRCDIR)/alloc.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build sha1 object
$(BUILDDIR)/sha1.o: $(SRCDIR)/vendor/sha1.c $(SRCDIR)/vendor/sha1.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build sha256 object
$(BUILDDIR)/sha256.o: $(SRCDIR)/vendor/sha256.c $(SRCDIR)/vendor/sha256.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build miniz object
$(BUILDDIR)/miniz.o: $(SRCDIR)/vendor/miniz.c $(SRCDIR)/vendor/miniz.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build librulr objects
$(BUILDDIR)/rulr/%.o: $(SRCDIR)/rulr/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(RULR_CFLAGS) -c $< -o $@

# Build librulr archive
$(RULR_LIB): $(RULR_OBJECTS) | $(BINDIR)
	$(AR) rcs $@ $(RULR_OBJECTS)

# Build rulr demo driver (optional helper for development)
$(RULR_DRIVER): $(RULR_DRIVER_OBJ) $(RULR_LIB) $(BUILDDIR)/alloc.o $(BUILDDIR)/miniz.o | $(BINDIR)
	$(CC) $(RULR_DRIVER_OBJ) $(RULR_LIB) $(BUILDDIR)/alloc.o $(BUILDDIR)/miniz.o -o $@

# Build rulrc compiler
$(RULRC_OBJ): $(RULRC_SRC) | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(RULR_CFLAGS) -c $< -o $@

$(RULRC): $(RULRC_OBJ) $(RULR_LIB) $(BUILDDIR)/alloc.o $(BUILDDIR)/miniz.o | $(BINDIR)
	$(CC) $(RULRC_OBJ) $(RULR_LIB) $(BUILDDIR)/alloc.o $(BUILDDIR)/miniz.o -o $@

# Link final binary
$(TARGET): $(OBJECTS) $(RULR_LIB) | $(BINDIR)
	$(CC) $(OBJECTS) $(RULR_LIB) $(LDFLAGS) -o $@

# Create directories
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(BUILDDIR) $(BINDIR)

pg_core_test: $(PG_CORE_TEST)

$(PG_CORE_TEST): $(BUILDDIR)/pg_core_test.o $(BUILDDIR)/pg_core.o $(BUILDDIR)/alloc.o $(BUILDDIR)/log.o | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

pg_file_test: $(PG_FILE_TEST)

$(PG_FILE_TEST): $(BUILDDIR)/pg_file_test.o $(BUILDDIR)/pg_core.o $(BUILDDIR)/alloc.o $(BUILDDIR)/log.o | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

test: pg_core_test pg_file_test
	@echo "Running pg_core_test..."
	@$(PG_CORE_TEST)
	@echo ""
	@echo "Running pg_file_test..."
	@$(PG_FILE_TEST) test/data

# Print current version
.PHONY: version
version:
	@$(MAKE) -f buildinfo.mk print-version

# Alias for test target - GitHub Actions compatibility
check: test

.PHONY: librulr rulr-demo
librulr: $(RULR_LIB)

rulr-demo: $(RULR_DRIVER)

# Create distribution tarball
dist:
	@echo "Creating distribution tarball..."
	@VERSION=$$(cat $(VERSION_FILE) | tr -d '\n'); \
	DIST_NAME=wrap-$$VERSION; \
	DIST_DIR=/tmp/wrap-dist-$$$$/$$DIST_NAME; \
	rm -rf /tmp/wrap-dist-$$$$; \
	mkdir -p $$DIST_DIR; \
	cp -r $(SRCDIR) $(VERSION_FILE) Makefile buildinfo.mk elm.json README.md $$DIST_DIR/; \
	if [ -d test ]; then cp -r test $$DIST_DIR/; fi; \
	if [ -d doc ]; then cp -r doc $$DIST_DIR/; fi; \
	if [ -d examples ]; then cp -r examples $$DIST_DIR/; fi; \
	if [ -f AGENTS.md ]; then cp AGENTS.md $$DIST_DIR/; fi; \
	tar czf $$DIST_NAME.tar.gz -C /tmp/wrap-dist-$$$$ $$DIST_NAME; \
	rm -rf /tmp/wrap-dist-$$$$; \
	echo "Created $$DIST_NAME.tar.gz"

# Build and test distribution tarball
distcheck: dist
	@echo "Running distcheck..."
	@VERSION=$$(cat $(VERSION_FILE) | tr -d '\n'); \
	DIST_NAME=wrap-$$VERSION; \
	DIST_TARBALL=$$DIST_NAME.tar.gz; \
	DISTCHECK_DIR=/tmp/distcheck-$$$$; \
	mkdir -p $$DISTCHECK_DIR; \
	echo "Extracting $$DIST_TARBALL to $$DISTCHECK_DIR..."; \
	tar xzf $$DIST_TARBALL -C $$DISTCHECK_DIR; \
	echo "Building in $$DISTCHECK_DIR/$$DIST_NAME..."; \
	cd $$DISTCHECK_DIR/$$DIST_NAME && $(MAKE) all && $(MAKE) check; \
	if [ $$? -eq 0 ]; then \
		echo "distcheck PASSED"; \
		rm -rf $$DISTCHECK_DIR; \
		exit 0; \
	else \
		echo "distcheck FAILED"; \
		rm -rf $$DISTCHECK_DIR; \
		exit 1; \
	fi

# Install to system directory (default: /usr/local/bin)
install: $(TARGET)
	@echo "Installing $(TARGET_FILE) to $(BINDIR_INSTALL)..."
	@install -d $(BINDIR_INSTALL)
	@install -m 755 $(TARGET) $(BINDIR_INSTALL)/$(TARGET_FILE)
	@echo "Installation complete. $(TARGET_FILE) installed to $(BINDIR_INSTALL)/$(TARGET_FILE)"

# Install to user's local directory
install-user: $(TARGET)
	@echo "Installing $(TARGET_FILE) to $(USER_BINDIR)..."
	@install -d $(USER_BINDIR)
	@install -m 755 $(TARGET) $(USER_BINDIR)/$(TARGET_FILE)
	@echo "Installation complete. $(TARGET_FILE) installed to $(USER_BINDIR)/$(TARGET_FILE)"
	@echo "Make sure $(USER_BINDIR) is in your PATH"

# Uninstall from system directories
uninstall:
	@echo "Uninstalling $(TARGET_FILE) from $(BINDIR_INSTALL)..."
	@rm -f $(BINDIR_INSTALL)/$(TARGET_FILE)
	@echo "Uninstall complete."

# Uninstall from user's local directory
uninstall-user:
	@echo "Uninstalling $(TARGET_FILE) from $(USER_BINDIR)..."
	@rm -f $(USER_BINDIR)/$(TARGET_FILE)
	@echo "Uninstall complete."
