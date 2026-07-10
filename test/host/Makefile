CC      ?= cc
CORE    := ../../components/core
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -I$(CORE)/include -I.
SRC     := $(CORE)/wav.c $(CORE)/textnorm.c $(CORE)/keymap.c $(CORE)/keymap_us.c \
           $(CORE)/multipart.c $(CORE)/sm.c $(CORE)/formdec.c $(CORE)/dnsreply.c $(CORE)/jsonesc.c $(CORE)/typeplan.c
TESTS   := test_wav test_textnorm test_keymap test_multipart test_sm test_formdec test_dnsreply test_jsonesc test_typeplan

.PHONY: test interop test-linux clean

# CI runs on glibc, which hides POSIX extensions under -std=c11 (the flag
# defines __STRICT_ANSI__) that Apple's libm exposes regardless. Run this
# before pushing if you touched host code: same compiler, same libc as the
# workflow. M_PI got through a green Mac build once already.
test-linux:
	@docker run --rm -u "$$(id -u):$$(id -g)" -e HOME=/tmp \
	  -v "$$(cd ../.. && pwd):/w" -w /w espressif/idf:v5.5 \
	  bash -lc 'make -C test/host clean && make -C test/host test'

test: $(TESTS) gen_fixtures
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; \
	 python3 interop.py || fail=1; \
	 if [ $$fail -eq 0 ]; then echo "--- all host tests passed ---"; \
	 else echo "--- HOST TESTS FAILED ---"; fi; exit $$fail

# Judged by parsers that did not write the bytes: python's wave + email.
interop: gen_fixtures
	@python3 interop.py

gen_fixtures: gen_fixtures.c $(SRC)
	@$(CC) $(CFLAGS) -o $@ $< $(SRC) -lm

$(TESTS): %: %.c $(SRC) test_util.h
	@$(CC) $(CFLAGS) -o $@ $< $(SRC)

clean:
	rm -f $(TESTS) gen_fixtures
