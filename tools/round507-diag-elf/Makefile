EE_BIN = diag.elf
EE_OBJS = diag.o
EE_LIBS = -ldebug -lcdvd -lc

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
