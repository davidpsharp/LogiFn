# LogiFn: builds the module. Runs inside the GCCSDK container; use tools/build
# from the host.
#
#   make            -> build/LogiFn,ffa

CC      = arm-unknown-riscos-gcc
CMUNGE  = cmunge
OUT     = build
CFLAGS  = -mlibscl -mmodule -O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
          -fno-strict-aliasing -Isrc -I$(OUT)

.PHONY: all clean
all: $(OUT)/LogiFn,ffa

$(OUT)/ModHdr.o $(OUT)/ModHdr.h: src/ModHdr.cmhg
	@mkdir -p $(OUT)
	$(CMUNGE) -tgcc -32bit -o $(OUT)/ModHdr.o -d $(OUT)/ModHdr.h $<

$(OUT)/logifn.o: src/logifn.c $(OUT)/ModHdr.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/LogiFn,ffa: $(OUT)/ModHdr.o $(OUT)/logifn.o
	$(CC) -mlibscl -mmodule -o $@ $^
	@echo "built $@"

clean:
	rm -rf $(OUT)
