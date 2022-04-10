#

# ソースの依存関係ファイル (自動生成)
DEPS = Dependencies
DEPENDENCIES_OUTPUT := $(DEPS)

# 標準ルール
include ../../etc/makerules

#-----------------------------------------------------------------------------
TARGET = txf

# ソースファイルのサーチパス
S = ../src
VPATH = $S

HEADER := $(S) $(HEADER)

# ソースファイル
SRC = txf.c

OBJ	= $(addsuffix .o, $(basename $(SRC)))
WC_SRC	= $(filter %.C, $(SRC))

# 追加ライブラリ
LOADLIBES = -lapp

# コンパイルオプション
CFLAGS += -Wall

#-----------------------------------------------------------------------------
.PHONY: all install clean

ALL = $(TARGET) $(addsuffix .map, $(TARGET))

all:	$(ALL)

$(TARGET): $(OBJ)
	$(LINK.o) $(LDOBJS) $^ $(LOADLIBES) $(LDLIBS) $(OUTPUT_OPTION)

clean:
	$(RM) $(OBJ) $(WC_SRC:%.C=%.c) $(ALL) $(DEPS)

install: $(addprefix $(EXE_INSTALLDIR)/, $(ALL))

$(TARGET:%=$(EXE_INSTALLDIR)/%): $(EXE_INSTALLDIR)/%: %
	$(EXE_INSTALL_STRIP)

# ソースの依存関係
$(WC_SRC:%.C=%.c):

$(OBJ):

ifdef DEPENDENCIES_OUTPUT
  $(DEPS): ; touch $(DEPS)
else
  $(DEPS): $(SRC) ; $(MAKEDEPS) $@ $?
endif
include $(DEPS)
