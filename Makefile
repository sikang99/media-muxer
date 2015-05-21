# the compiler: gcc for C program, define as g++ for C++
CC = c++
CFLAGS  = -g -Wall -O2
TARGET = muxer
INCLUDES = -I../libav-11.3
LDFLAGS = -L../libav-11.3/libavcodec -L../libav-11.3/libavformat -L../libav-11.3/libavutil -L../libav-11.3/libavresample -L../../build/lib
LIBS = -lavformat -lavcodec -lavutil -lavresample -lz -lbz2 -lboost_system -lboost_thread -pthread -lfdk-aac -lx264

all: $(TARGET)

$(TARGET): $(TARGET).cc
	$(CC) $(INCLUDES) $(CFLAGS) -o $(TARGET) $(TARGET).cc $(LDFLAGS) $(LIBS)

clean:
	$(RM) $(TARGET)

test: $(TARGET)
	LD_LIBRARY_PATH=../../build/lib ./$(TARGET)
