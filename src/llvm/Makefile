# Makefile — MCP4LLVM pipeline
#
# Targets:
#   make mcp4llvm          build the code generator binary
#   make PROG=hello run    compile hello.ll + runtime, link, run
#   make clean

CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LLC     = llc
LLFLAGS = -O2 -filetype=obj

# ---- Code generator --------------------------------------------------------

mcp4llvm: MCP4LLVM.c
	$(CC) $(CFLAGS) -o $@ $< -lm

# ---- Runtime library -------------------------------------------------------

m2runtime.o: m2runtime.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- Compile + link a generated .ll file -----------------------------------
# Usage: make PROG=hello run
#   expects hello.ll in current directory

PROG ?= out

$(PROG).o: $(PROG).ll
	$(LLC) $(LLFLAGS) $< -o $@

$(PROG): $(PROG).o m2runtime.o
	$(CC) -o $@ $^ -lm

run: $(PROG)
	./$(PROG)

# ---- Convenience: full pipeline from IL1 + ASC ----------------------------
# Usage: make IL1=prog.il1 ASC=prog.asc codegen
codegen: mcp4llvm m2runtime.o
	./mcp4llvm $(IL1) $(ASC) $(PROG).ll
	$(LLC) $(LLFLAGS) $(PROG).ll -o $(PROG).o
	$(CC) -o $(PROG) $(PROG).o m2runtime.o -lm
	@echo "Built: $(PROG)"

# ---- Housekeeping ----------------------------------------------------------

clean:
	rm -f mcp4llvm mcp4llvm.exe m2runtime.o *.o *.ll $(PROG)

.PHONY: run codegen clean
