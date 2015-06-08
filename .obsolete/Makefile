# the compiler: gcc for C program, define as g++ for C++
CC = clang++
CFLAGS  = -g -Wall -O2 -std=c++11
TARGET = capture
INCLUDES =
LDFLAGS =
LIBS = -lavformat -lavcodec -lavutil -lavdevice -lswresample -lswscale

all: video audio

$(TARGET): $(TARGET).cc
	$(CC) $(INCLUDES) $(CFLAGS) -DMAIN_PROGRAM -o $(TARGET) $(TARGET).cc $(LDFLAGS) $(LIBS)

clean:
	$(RM) $(TARGET) $(TARGET)_audio $(TARGET)_video

video: $(TARGET).cc
	$(CC) $(INCLUDES) $(CFLAGS) -DMAIN_PROGRAM -o $(TARGET)_$@ $(TARGET).cc $(LDFLAGS) $(LIBS)

audio: $(TARGET).cc
	$(CC) $(INCLUDES) $(CFLAGS) -DMAIN_PROGRAM -DCAP_AUDIO -o $(TARGET)_$@ $(TARGET).cc $(LDFLAGS) $(LIBS)
