
CC=gcc

fltk-config = fltk-config

# Different executables, and -lopengl32 is required for Windows
ifeq ($(OS),Windows_NT)
EDIT_BIN = ../bin/editor.exe
LFLAGS += `sdl2-config --libs` -lopengl32
RES = rengine.res
else
EDIT_BIN = ../bin/editor
LFLAGS += `sdl2-config --libs` -lGL
RES = 
endif

CFLAGS = -I ../include -I ..
CPPFLAGS = `$(fltk-config) --cxxflags` -c -I . -I./editor -I ../include
LPPFLAGS = `$(fltk-config) --ldflags` 

ifeq ($(BUILD),debug)
# Debug mode: Unoptimized and with debugging symbols
CPPFLAGS += -Wall -O0 -g
LPPFLAGS += 
else
	ifeq ($(BUILD),profile)
	# Profile mode: Debugging symbols and profiling information.
	CPPFLAGS += -Wall -O0 -pg
	LPPFLAGS += -pg
	else
	# Release mode: Optimized and stripped of debugging info
	CPPFLAGS += -Wall -Os -DNDEBUG
	LPPFLAGS += -s 
	endif
endif

# Link with static libstdc++, otherwise you need to have
# libstdc++-6.dll around.
LPPFLAGS += -static-libstdc++

.PHONY : editor

editor: $(EDIT_BIN)

$(EDIT_BIN): main.o editor.o BMCanvas.o LevelCanvas.o TileCanvas.o \
				bmp.o tileset.o map.o lexer.o json.o hash.o utils.o
	g++ -o $@  $^ $(LPPFLAGS)
	
editor.o : editor.cxx editor.h 
	g++ -c $(CPPFLAGS) $< -o $@

editor.cxx editor.h : editor.fl
	fluid -c $^
	
BMCanvas.o: BMCanvas.cpp BMCanvas.h ../include/bmp.h
	g++ -c $(CPPFLAGS) $< -o $@

LevelCanvas.o: LevelCanvas.cpp LevelCanvas.h TileCanvas.h \
				../include/bmp.h ../include/tileset.h ../include/map.h ../include/utils.h
	g++ -c $(CPPFLAGS) $< -o $@
	
TileCanvas.o: TileCanvas.cpp TileCanvas.h ../include/bmp.h ../include/tileset.h
	g++ -c $(CPPFLAGS) $< -o $@
	
main.o: main.cpp LevelCanvas.h BMCanvas.h \
		TileCanvas.h TileCanvas.h editor.h ../include/bmp.h ../include/tileset.h ../include/map.h
	g++ -c $(CPPFLAGS) $< -o $@

bmp.o  : ../src/bmp.c ../include/bmp.h
	$(CC) -c $(CFLAGS) $< -o $@
tileset.o: ../src/tileset.c ../include/tileset.h
	$(CC) -c $(CFLAGS) $< -o $@
map.o: ../src/map.c ../include/map.h
	$(CC) -c $(CFLAGS) $< -o $@
lexer.o: ../src/lexer.c ../include/lexer.h
	$(CC) -c $(CFLAGS) $< -o $@
json.o: ../src/json.c ../include/json.h
	$(CC) -c $(CFLAGS) $< -o $@
hash.o: ../src/hash.c ../include/hash.h
	$(CC) -c $(CFLAGS) $< -o $@
utils.o: ../src/utils.c ../include/utils.h
	$(CC) -c $(CFLAGS) $< -o $@

.PHONY : clean

clean:
	-rm -rf $(EDIT_BIN)
	-rm -rf *.o editor.cxx editor.h
	-rm -rf *~ gmon.out