package main

// #cgo darwin LDFLAGS: -L../libav-11.3/libavcodec -L../libav-11.3/libavformat -L../libav-11.3/libavutil -L../libav-11.3/libavresample -lavformat -lavcodec -lavutil -lavresample -lz -lbz2 -lfdk-aac -lx264 -lvpx -lopus
import "C"
import (
	"./mux"
)

func main() {
	m, err := mux.NewMuxer("rtsp", "rtsp://localhost:1935/live/bundle.sdp")
	if err == nil && m.Start() {
		m.WaitForDone()
	}
}
