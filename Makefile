CC = gcc
EXEC = wayst
VERSION = "0.1.0"
INSTALL_DIR = /usr/local/bin

ARGS =

SRC_DIR = src
BLD_DIR = build
TGT_DIR = .

LDLIBS = -lGL -lfreetype -lfontconfig -lutil -L/usr/lib -lm
INCLUDES = -I"/usr/include/freetype2/"


ifeq ($(mode),debug)
	CFLAGS = -O0 -g3 -ffinite-math-only -fno-rounding-math -std=c18 -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -DDEBUG -DVERSION="\"${VERSION} debug build\""
	LDFLAGS =  -fsanitize=address -fsanitize=undefined -fsanitize=unreachable -fno-omit-frame-pointer
	LDLIBS += -lGLU
else ifeq ($(mode),debugoptimized)
	CFLAGS = -std=c18 -s -O2 -ftree-loop-vectorize -mtune=generic -ffast-math -mfpmath=sse -DDEBUG -DVERSION="\"${VERSION} debug build\""
	LDFLAGS = -O2 -g
	LDLIBS += -lGLU
else
	CFLAGS = -std=c18 -s -O3 -fomit-frame-pointer -mtune=native -ffast-math -mfpmath=sse -DVERSION=\"${VERSION}\"
	LDFLAGS = -s -O2 -flto
endif

CCWNO = -Wall -Wextra -Wno-unused-parameter -Wno-address -Wno-unused-function -Werror=implicit-function-declaration

SRCS = $(wildcard $(SRC_DIR)/*.c wildcard $(SRC_DIR)/wcwidth/wcwidth.c)
SRCS_WLEXTS = $(wildcard $(SRC_DIR)/wl_exts/*.c)

XLDLIBS = -lX11 -lXrandr -lXrender -lEGL
WLLDLIBS = -lwayland-client -lwayland-egl -lwayland-cursor -lxkbcommon 

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
	$(CC) $(OBJ) $(LDLIBS) -o $(TGT_DIR)/$(EXEC) $(LDFLAGS) $(INCLUDES) $(CCWNO)

all: $(OBJ)
	$(CC) $(OBJ) $(LDLIBS) -o $(TGT_DIR)/$(EXEC) $(LDFLAGS) $(INCLUDES) $(CCWNO)

$(BLD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) -c $< $(CFLAGS) $(CCWNO) $(INCLUDES) -o $@

run:
	@cd $(TGT_DIR);./$(EXEC) $(ARGS)

debug:
	@cd $(TGT_DIR); gdb --args ./$(EXEC) $(ARGS)

clean:
	$(RM) -f $(OBJ)

cleanall:
	$(RM) -f $(EXEC) $(OBJ)

install:
	@cp $(EXEC) $(INSTALL_DIR)/
