CC?= cc
EXEC = wayst
INSTALL_DIR = /usr/local/bin

ARGS =

SRC_DIR = src
BLD_DIR = build
TGT_DIR = .

LDLIBS = -lGL -lfreetype -lfontconfig -lutil -L/usr/lib -lm

ifeq ($(shell uname -s),FreeBSD)
	INCLUDES = -I/usr/local/include/freetype2/
else
	INCLUDES = -I/usr/include/freetype2/
endif

ifeq ($(mode),debug)
	CFLAGS = -O0 -g3 -ffinite-math-only -fno-rounding-math -std=c18 -fsanitize=address -fsanitize=undefined -DDEBUG -DVERSION="\"debug build\""
	LDFLAGS =  -fsanitize=address -fsanitize=undefined -fsanitize=unreachable
	LDLIBS += -lGLU
else ifeq ($(mode),debugoptimized)
	CFLAGS = -std=c18 -g -O2 -fno-omit-frame-pointer -mtune=generic -ffast-math -DDEBUG -DVERSION="\"debug build\""
	LDFLAGS = -O2 -g
	LDLIBS += -lGLU
else
	CFLAGS = -std=c18 -O2 -mtune=generic -ffast-math
	LDFLAGS = -s -O2 -flto
endif

CCWNO = -Wall -Wextra -Wno-unused-parameter -Wno-address -Wno-unused-function -Werror=implicit-function-declaration

SRCS = $(wildcard $(SRC_DIR)/*.c wildcard $(SRC_DIR)/wcwidth/wcwidth.c)
SRCS_WLEXTS = $(wildcard $(SRC_DIR)/wl_exts/*.c)

XLDLIBS = -lX11 -lXrandr -lXrender
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
	$(RM) -f $(EXEC) $(OBJ)

install:
	@cp $(EXEC) $(INSTALL_DIR)/
