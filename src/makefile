# Executable name
EXE = Grizzly.exe

# Compiler
CC = gcc

# Source files - UPDATED (8 files total with tt.c added)
# Core files:
#   board.c     - Board representation, move execution, FEN parsing
#   eval.c  	- Position evaluation (PSQ, pawns, KPK bitbase, endgames)
#   main.c      - Entry point and initialization
#   move.c  - Move generation
#   search.c    - Search algorithm + move picker + history
#   tt.c        - Transposition table
#   worker.c    - Worker pool + time management + iterative deepening
#   uci.c       - UCI protocol + option handling

SOURCES 	= board.c nn.c main.c move.c search.c worker.c uci.c

# Object files
OBJECTS = $(SOURCES:.c=.o)

# Default flags
CFLAGS = -Wall -Wextra -std=gnu11 -DNDEBUG
LDFLAGS = -lpthread -lm

# Optimization flags
OPT_FLAGS = -O3 -mtune=generic

# Windows-specific settings
ifeq ($(OS),Windows_NT)
    RM = del /Q
    RMDIR = rmdir /S /Q
    MKDIR = mkdir
    NULL = NUL
    PATHSEP = \\
else
    RM = rm -f
    RMDIR = rm -rf
    MKDIR = mkdir -p
    NULL = /dev/null
    PATHSEP = /
endif

# Default target
.DEFAULT_GOAL := release

# Release build (default)
release: CFLAGS += $(OPT_FLAGS)
release: $(EXE)
release: clean_obj

# Debug build
debug: CFLAGS = -Wall -Wextra -std=gnu11 -g -O0 -DDEBUG
debug: $(EXE)
debug: clean_obj

# Profile build
profile: CFLAGS += $(OPT_FLAGS) -pg
profile: LDFLAGS += -pg
profile: $(EXE)
profile: clean_obj

# Tuning build (requires OpenMP) - note: tuner.c removed, needs to be added back for tuning
tune: CFLAGS = -Wall -Wextra -std=gnu11 -DTUNE -DNDEBUG $(OPT_FLAGS) -fopenmp
tune: LDFLAGS += -fopenmp
tune: $(EXE)
tune: clean_obj

# Modern x64 with BMI2 and POPCNT
modern: CFLAGS += $(OPT_FLAGS) -mbmi2 -mpopcnt -DUSE_PEXT
modern: $(EXE)
modern: clean_obj

# BMI2 build
bmi2: CFLAGS += $(OPT_FLAGS) -mbmi2 -DUSE_PEXT
bmi2: $(EXE)
bmi2: clean_obj

# POPCNT build
popcnt: CFLAGS += $(OPT_FLAGS) -mpopcnt
popcnt: $(EXE)
popcnt: clean_obj

# Static build (for portability)
static: CFLAGS += $(OPT_FLAGS)
static: LDFLAGS += -static
static: $(EXE)
static: clean_obj

# Build executable
$(EXE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $(EXE)
	@echo Built $(EXE) successfully

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean object files only (after build)
clean_obj:
	@$(RM) *.o 2>$(NULL) || true
	@echo Cleaned object files

# Clean build artifacts (verbose)
clean:
	-$(RM) *.o
	-$(RM) $(EXE)
	@echo Cleaned build artifacts

# Clean everything including backup files
distclean: clean
	-$(RM) *~ *.bak
	@echo Cleaned all temporary files

# Help target
help:
	@echo CC - Build Targets:
	@echo   make release    - Build optimized release version (default)
	@echo   make debug      - Build debug version with symbols
	@echo   make profile    - Build with profiling enabled
	@echo   make tune       - Build tuning version (requires OpenMP + tuner.c)
	@echo   make modern     - Build with BMI2 + POPCNT
	@echo   make bmi2       - Build with BMI2
	@echo   make popcnt     - Build with POPCNT
	@echo   make static     - Build static executable
	@echo   make clean      - Remove build artifacts
	@echo   make distclean  - Remove all temporary files
	@echo.
	@echo SOURCE FILE ORGANIZATION (8 files total):
	@echo   board.c     - Board representation, move execution, FEN parsing
	@echo   eval.c  - Position HCE evaluation (PSQ, pawns, KPK, endgames)
	@echo   main.c      - Entry point and initialization
	@echo   move.c  - Move generation (captures, quiets, evasions)
	@echo   search.c    - Search algorithm, move picker, history
	@echo   tt.c        - Transposition table
	@echo   worker.c    - Worker pool, time management, iterative deepening
	@echo   uci.c       - UCI protocol and option handling
	@echo.
	@echo KEY CHANGES FROM PREVIOUS VERSION:
	@echo   - Separated tt.c from search.c for better organization
	@echo   - Separated worker.c from search.c for better organization
	@echo   - search.c now focused on core search algorithm
	@echo   - tt.c handles transposition table operations
	@echo   - worker.c handles threading, time management, and coordination
	@echo.
	@echo REMOVED FILES (optional features):
	@echo   - bench.c (benchmarking - can be re-added if needed)
	@echo   - tuner.c (parameter tuning - can be re-added if needed)
	@echo.
	@echo Compiler: $(CC)

# Phony targets
.PHONY: release debug profile tune modern bmi2 popcnt static clean clean_obj distclean help