EXEC = wayst
INSTALL_DIR = /usr/local/bin

ARGS =

SRC_DIR = src
TST_DIR = tests/unit_tests
BLD_DIR = build
TGT_DIR = .

ifeq ($(shell uname -s),FreeBSD)
	INCLUDES = -I/usr/local/include/freetype2/
	INCLUDES += -I/usr/local/include
	LDLIBS = -lGL -lfreetype -lfontconfig -lutil -L/usr/local/lib -lm
else
	CC?= cc
	INCLUDES = -I/usr/include/freetype2/
	LDLIBS = -lGL -lfreetype -lfontconfig -lutil -L/usr/lib -lm
endif

ifeq ($(mode),sanitized)
	CFLAGS = -std=c18 -MD -O0 -g3 -ffinite-math-only -fno-rounding-math -fshort-enums -fsanitize=address -fsanitize=undefined -DDEBUG
	LDFLAGS =  -fsanitize=address -fsanitize=undefined -fsanitize=unreachable
	LDLIBS += -lGLU
else ifeq ($(mode),debug)
	CFLAGS = -std=c18 -MD -g -O0 -fno-omit-frame-pointer -fshort-enums -DDEBUG
	LDFLAGS = -O0 -g
	LDLIBS += -lGLU
else ifeq ($(mode),profile)
	CFLAGS = -std=c18 -O2 -mtune=generic -ffast-math -fshort-enums -pg -fno-omit-frame-pointer
	LDFLAGS = -O2 -pg
else ifeq ($(mode),debugoptimized)
	CFLAGS = -std=c18 -MD -g -O2 -fno-omit-frame-pointer -mtune=generic -ffast-math -fshort-enums -DDEBUG
	LDFLAGS = -O2 -g
	LDLIBS += -lGLU
else
	CFLAGS = -std=c18 -MD -O2 -mtune=generic -ffast-math -fshort-enums -flto=auto
	LDFLAGS = -O2 -flto=auto
endif

ifeq ($(libutf8proc),off)
$(info libutf8proc not found. Support for language-specific combining characters and unicode normalization will be disabled.)
	CFLAGS += -DNOUTF8PROC
else
	LDLIBS += -lutf8proc
endif

CCWNO = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Werror=implicit-function-declaration

SRCS = $(wildcard $(SRC_DIR)/*.c wildcard $(SRC_DIR)/wcwidth/wcwidth.c)
SRCS_WLEXTS = $(wildcard $(SRC_DIR)/wl_exts/*.c)

XLDLIBS = -lX11 -lXrender -lXrandr
WLLDLIBS = -lwayland-client -lwayland-egl -lwayland-cursor -lxkbcommon -lEGL

ifeq ($(window_protocol), x11)
	CFLAGS += -DNOWL
	OBJ = $(SRCS:$(SRC_DIR)/%.c=$(BLD_DIR)/%.o)
	LDLIBS += $(XLDLIBS)
else ifeq ($(window_protocol), wayland)
	CFLAGS += -DNOX
	OBJ = $(SRCS:$(SRC_DIR)/%.c=$(BLD_DIR)/%.o) $(SRCS_WLEXTS:$(SRC_DIR)/%.c=$(BLD_DIR)/%.o)
	LDLIBS += $(WLLDLIBS)
else
	OBJ = $(SRCS:$(SRC_DIR)/%.c=$(BLD_DIR)/%.o) $(SRCS_WLEXTS:$(SRC_DIR)/%.c=$(BLD_DIR)/%.o)
	LDLIBS += $(XLDLIBS) $(WLLDLIBS)
endif

ifeq ($(renderer), gles20)
	CFLAGS += -DGFX_GLES
	LDLIBS += -lGLESv2
else
	LDLIBS += -lGL
endif


$(EXEC): $(OBJ)
	$(CC) $(OBJ) $(LDLIBS) -o $(TGT_DIR)/$(EXEC) $(LDFLAGS)

all: $(OBJ)
	$(CC) $(OBJ) $(LDLIBS) -o $(TGT_DIR)/$(EXEC) $(LDFLAGS)

$(BLD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BLD_DIR)
	@mkdir -p $(BLD_DIR)/wcwidth
	@mkdir -p $(BLD_DIR)/wl_exts
	$(CC) -c $< $(CFLAGS) $(CCWNO) $(INCLUDES) -o $@

run:
	./$(TGT_DIR)/$(EXEC) $(ARGS)

debug:
	gdb --args ./$(TGT_DIR)/$(EXEC) $(ARGS)

clean:
	$(RM) -f $(OBJ)

cleanall:
	$(RM) -f $(EXEC) $(OBJ) $(OBJ:.o=.d)

install:
	@cp $(EXEC) $(INSTALL_DIR)/

uninstall:
	$(RM) $(INSTALL_DIR)/$(EXEC)

-include $(OBJ:.o=.d)


.PHONY: shaders
.PHONY: test
.PHONY: graph

shaders:
	./$(SRC_DIR)/pack_shaders.sh shaders_gl21 > $(SRC_DIR)/shaders_gl21.h
	./$(SRC_DIR)/pack_shaders.sh shaders_gles20 > $(SRC_DIR)/shaders_gles20.h

test:
	@chmod +x $(TST_DIR)/run_tests.sh
	@./$(TST_DIR)/run_tests.sh

graph:
	@gprof ./wayst | gprof2dot | dot -Tpng -o perf_graph.png
