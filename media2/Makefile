# the compiler: gcc for C program, define as g++ for C++
CC = g++
CFLAGS  = -Wall -O2 -std=c++11
INCLUDES =
LDFLAGS =
LIBS = -lavformat -lavcodec -lavutil -lavdevice -lswresample -lswscale -lboost_system -lSDL2
SRC = capture.cc muxer.cc player.cc main.cc


ifeq ($(shell uname),Darwin)
	LIBS += -lboost_thread-mt
else
	LIBS += -lboost_thread
endif

all: muxer

clean:
	$(RM) muxer
	$(RM) -r *.dSYM

debug: CFLAGS += -g
debug: muxer

muxer: $(SRC)
	$(CC) $(INCLUDES) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LIBS)
