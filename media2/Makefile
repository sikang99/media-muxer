# the compiler: gcc for C program, define as g++ for C++
CC = clang++
CFLAGS  = -Wall -O2 -std=c++11
INCLUDES =
LDFLAGS =
LIBS = -lavformat -lavcodec -lavutil -lavdevice -lswresample -lswscale
SRC = capture.cc muxer.cc main.cc

all: video audio

clean:
	$(RM) muxer muxer_audio muxer_video
	$(RM) -r *.dSYM

debug: CFLAGS += -g
debug: video audio

video: $(SRC)
	$(CC) $(INCLUDES) $(CFLAGS) -o muxer_$@ $(SRC) $(LDFLAGS) $(LIBS)

audio: $(SRC)
	$(CC) $(INCLUDES) $(CFLAGS) -DCAP_AUDIO -o muxer_$@ $(SRC) $(LDFLAGS) $(LIBS)
