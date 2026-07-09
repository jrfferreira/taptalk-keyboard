CC      ?= cc
CORE    := ../../components/core
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -I$(CORE)/include -I.
SRC     := $(CORE)/wav.c $(CORE)/textnorm.c $(CORE)/keymap.c $(CORE)/keymap_us.c \
           $(CORE)/multipart.c $(CORE)/sm.c $(CORE)/formdec.c $(CORE)/dnsreply.c
TESTS   := test_wav test_textnorm test_keymap test_multipart test_sm test_formdec test_dnsreply

.PHONY: test interop clean
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
