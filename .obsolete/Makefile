# the compiler: gcc for C program, define as g++ for C++
CC = c++
CFLAGS  = -g -Wall -O2
TARGET = muxer
INCLUDES =
LDFLAGS =
LIBS = -lavformat -lavcodec -lavutil -lavdevice -lswscale -lboost_system -lSDL

ifeq ($(shell uname),Darwin)
	LIBS += -lboost_thread-mt
else
	LIBS += -lboost_thread
endif

all: $(TARGET)

$(TARGET): $(TARGET).cc
	$(CC) $(INCLUDES) $(CFLAGS) -o $(TARGET) $(TARGET).cc $(LDFLAGS) $(LIBS)

clean:
	$(RM) $(TARGET)

test: $(TARGET)
	LD_LIBRARY_PATH=../../build/lib ./$(TARGET)
