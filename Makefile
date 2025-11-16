CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -D_POSIX_C_SOURCE=200809L -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -O2
LDFLAGS =

# Directories
SRCDIR = src
BUILDDIR = build
BINDIR = bin

# Files
TARGET_FILE = elm-wrap
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/alloc.c \
          $(SRCDIR)/log.c \
          $(SRCDIR)/progname.c \
          $(SRCDIR)/init.c \
          $(SRCDIR)/make.c \
          $(SRCDIR)/repl.c \
          $(SRCDIR)/reactor.c \
          $(SRCDIR)/bump.c \
          $(SRCDIR)/diff.c \
          $(SRCDIR)/publish.c \
          $(SRCDIR)/install.c \
          $(SRCDIR)/install_check.c \
          $(SRCDIR)/elm_json.c \
          $(SRCDIR)/cache.c \
          $(SRCDIR)/solver.c \
          $(SRCDIR)/cJSON.c \
          $(SRCDIR)/http_client.c \
          $(SRCDIR)/registry.c \
          $(SRCDIR)/install_env.c \
          $(SRCDIR)/fileutil.c \
          $(SRCDIR)/pgsolver/pg_core.c \
          $(SRCDIR)/pgsolver/pg_elm.c \
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
          $(BUILDDIR)/install.o \
          $(BUILDDIR)/install_check.o \
          $(BUILDDIR)/elm_json.o \
          $(BUILDDIR)/cache.o \
          $(BUILDDIR)/solver.o \
          $(BUILDDIR)/cJSON.o \
          $(BUILDDIR)/http_client.o \
          $(BUILDDIR)/registry.o \
          $(BUILDDIR)/install_env.o \
          $(BUILDDIR)/fileutil.o \
          $(BUILDDIR)/pg_core.o \
          $(BUILDDIR)/pg_elm.o \
          $(BUILDDIR)/sha1.o \
          $(BUILDDIR)/sha256.o \
          $(BUILDDIR)/miniz.o \
          $(BUILDDIR)/buildinfo.o
TARGET = $(BINDIR)/$(TARGET_FILE)
PG_CORE_TEST = $(BINDIR)/pg_core_test
PG_FILE_TEST = $(BINDIR)/pg_file_test
VERSION_FILE = VERSION

# Include buildinfo.mk for version generation
include buildinfo.mk

# Ensure 'all' is the default target (must be set after include)
.DEFAULT_GOAL := all

.PHONY: all clean pg_core_test pg_file_test test check dist distcheck

all: $(TARGET)

# Generate buildinfo.c before compiling
$(BUILDINFO_SRC): buildinfo.mk $(VERSION_FILE)
	@$(MAKE) -f buildinfo.mk generate-buildinfo \
		BUILDDIR=$(BUILDDIR) \
		VERSION_FILE=$(VERSION_FILE) \
		CC=$(CC)

# Build buildinfo object
$(BUILDDIR)/buildinfo.o: $(BUILDINFO_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

# Build main object
$(BUILDDIR)/main.o: $(SRCDIR)/main.c $(SRCDIR)/buildinfo.h $(SRCDIR)/install.h $(SRCDIR)/make.h $(SRCDIR)/init.h $(SRCDIR)/repl.h $(SRCDIR)/reactor.h $(SRCDIR)/bump.h $(SRCDIR)/diff.h $(SRCDIR)/publish.h | $(BUILDDIR)
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
$(BUILDDIR)/init.o: $(SRCDIR)/init.c $(SRCDIR)/init.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build make object
$(BUILDDIR)/make.o: $(SRCDIR)/make.c $(SRCDIR)/make.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build repl object
$(BUILDDIR)/repl.o: $(SRCDIR)/repl.c $(SRCDIR)/repl.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build reactor object
$(BUILDDIR)/reactor.o: $(SRCDIR)/reactor.c $(SRCDIR)/reactor.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build bump object
$(BUILDDIR)/bump.o: $(SRCDIR)/bump.c $(SRCDIR)/bump.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build diff object
$(BUILDDIR)/diff.o: $(SRCDIR)/diff.c $(SRCDIR)/diff.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build publish object
$(BUILDDIR)/publish.o: $(SRCDIR)/publish.c $(SRCDIR)/publish.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/install_env.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build install object
$(BUILDDIR)/install.o: $(SRCDIR)/install.c $(SRCDIR)/install.h $(SRCDIR)/install_check.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h $(SRCDIR)/solver.h $(SRCDIR)/fileutil.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build install_check object
$(BUILDDIR)/install_check.o: $(SRCDIR)/install_check.c $(SRCDIR)/install_check.h $(SRCDIR)/elm_json.h $(SRCDIR)/cJSON.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build elm_json object
$(BUILDDIR)/elm_json.o: $(SRCDIR)/elm_json.c $(SRCDIR)/elm_json.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build cache object
$(BUILDDIR)/cache.o: $(SRCDIR)/cache.c $(SRCDIR)/cache.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build solver object
$(BUILDDIR)/solver.o: $(SRCDIR)/solver.c $(SRCDIR)/solver.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build PubGrub core object
$(BUILDDIR)/pg_core.o: $(SRCDIR)/pgsolver/pg_core.c $(SRCDIR)/pgsolver/pg_core.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build Elm adapter object
$(BUILDDIR)/pg_elm.o: $(SRCDIR)/pgsolver/pg_elm.c $(SRCDIR)/pgsolver/pg_elm.h $(SRCDIR)/pgsolver/pg_core.h $(SRCDIR)/elm_json.h $(SRCDIR)/cache.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

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

# Build install_env object
$(BUILDDIR)/install_env.o: $(SRCDIR)/install_env.c $(SRCDIR)/install_env.h $(SRCDIR)/cache.h $(SRCDIR)/registry.h $(SRCDIR)/http_client.h $(SRCDIR)/cJSON.h $(SRCDIR)/vendor/sha1.h $(SRCDIR)/fileutil.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build fileutil object
$(BUILDDIR)/fileutil.o: $(SRCDIR)/fileutil.c $(SRCDIR)/fileutil.h $(SRCDIR)/vendor/miniz.h | $(BUILDDIR)
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

# Link final binary
$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(OBJECTS) $(LDFLAGS) -lcurl -o $@

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
