#!/bin/bash

c++ muxer.cc -L../libav-11.3/libavcodec -L../libav-11.3/libavformat -L../libav-11.3/libavutil -L../libav-11.3/libavresample -lavformat -lavcodec -lavutil -lavresample -lz -lbz2 -lboost_system -lboost_thread -pthread -L../../build/lib -lfdk-aac -lx264 -o muxer -g -Wall

