# reginold/Makefile
#
# The standalone build vendors the exact Onigmo snapshot and generated config
# it depends on under vendor/onigmo/. No external Ruby checkout is required.

CC       = gcc
AR       = ar
REGINOLD_DIR = .
ONIGMO_DIR = $(REGINOLD_DIR)/vendor/onigmo
UNICODE_DIR = $(ONIGMO_DIR)/enc/unicode/17.0.0

CFLAGS = -Wall -Wextra -std=c11 -g

# ONIGMO_CFLAGS is INTERNAL to reginold's build only.
# It is never needed by consumers — they include only reginold.h,
# which has no Onigmo dependencies.
ONIGMO_CFLAGS = -DNOT_RUBY -D_GNU_SOURCE -I$(REGINOLD_DIR) -I$(ONIGMO_DIR) -I$(UNICODE_DIR)

# Use CFLAGS_INTERNAL for all reginold source files.
# Test binaries that exercise internals (test_classify, test_engine) also use this.
CFLAGS_INTERNAL = $(CFLAGS) $(ONIGMO_CFLAGS)

# Local non-Ruby Onigmo object files built from the Ruby source tree.
# Bundled into libreginold.a so consumers link one archive only.
ONIGMO_OBJS = \
	onig_regcomp.o   \
	onig_regparse.o  \
	onig_regexec.o   \
	onig_regenc.o    \
	onig_regerror.o  \
	onig_regsyntax.o \
	onig_st.o        \
	onig_ascii.o     \
	onig_unicode.o   \
	onig_utf_8.o

REGINOLD_OBJS = classify.o nfa.o exec.o engine.o reginold.o

.PHONY: all test clean

all: libreginold.a test_classify test_engine test_reginold

onig_regcomp.o: $(ONIGMO_DIR)/regcomp.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

onig_regparse.o: $(ONIGMO_DIR)/regparse.c
	$(CC) $(CFLAGS_INTERNAL) -DINTERNAL_SANITIZERS_H -include $(REGINOLD_DIR)/internal/sanitizers.h -c $< -o $@

onig_regexec.o: $(ONIGMO_DIR)/regexec.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

onig_regenc.o: $(ONIGMO_DIR)/regenc.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

onig_regerror.o: $(ONIGMO_DIR)/regerror.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

onig_regsyntax.o: $(ONIGMO_DIR)/regsyntax.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

onig_st.o: $(ONIGMO_DIR)/st.c
	$(CC) $(CFLAGS_INTERNAL) -DINTERNAL_SANITIZERS_H -include $(REGINOLD_DIR)/internal/sanitizers.h -c $< -o $@

onig_ascii.o: $(ONIGMO_DIR)/enc/ascii.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

onig_unicode.o: $(ONIGMO_DIR)/enc/unicode.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

onig_utf_8.o: $(ONIGMO_DIR)/enc/utf_8.c
	$(CC) $(CFLAGS_INTERNAL) -c $< -o $@

classify.o: classify.c classify.h onigmo_compat.h
	$(CC) $(CFLAGS_INTERNAL) -c classify.c -o classify.o

nfa.o: nfa.c nfa.h classify.h onigmo_compat.h
	$(CC) $(CFLAGS_INTERNAL) -c nfa.c -o nfa.o

exec.o: exec.c exec.h nfa.h classify.h
	$(CC) $(CFLAGS_INTERNAL) -c exec.c -o exec.o

engine.o: engine.c engine.h classify.h nfa.h exec.h onigmo_compat.h
	$(CC) $(CFLAGS_INTERNAL) -c engine.c -o engine.o

reginold.o: reginold.c reginold.h engine.h onigmo_compat.h
	$(CC) $(CFLAGS_INTERNAL) -c reginold.c -o reginold.o

# Static library for consumers (e.g. stoned).
# Bundles reginold objects + Onigmo objects — consumers link one archive
# and only need -I<reginold_dir> to resolve reginold.h.
libreginold.a: $(REGINOLD_OBJS) $(ONIGMO_OBJS)
	$(AR) rcs $@ $^

test_classify: test_classify.c classify.o $(ONIGMO_OBJS)
	$(CC) $(CFLAGS_INTERNAL) test_classify.c classify.o $(ONIGMO_OBJS) \
		-lm -ldl -lpthread \
		-o test_classify

test_engine: test_engine.c $(REGINOLD_OBJS) $(ONIGMO_OBJS)
	$(CC) $(CFLAGS_INTERNAL) test_engine.c $(REGINOLD_OBJS) $(ONIGMO_OBJS) \
		-lm -ldl -lpthread \
		-o test_engine

# test_reginold uses only $(CFLAGS) — no ONIGMO_CFLAGS needed.
# This confirms that consumers require no Onigmo include path.
test_reginold: test_reginold.c libreginold.a
	$(CC) $(CFLAGS) test_reginold.c libreginold.a \
		-lm -ldl -lpthread \
		-o test_reginold

test: test_classify test_engine test_reginold
	./test_classify
	./test_engine
	./test_reginold

clean:
	rm -f $(REGINOLD_OBJS) libreginold.a
	rm -f $(ONIGMO_OBJS)
	rm -f test_classify test_engine test_reginold
