package main

import (
	"./mux"
)

func main() {
	m, err := mux.NewMuxer("rtsp", "rtsp://localhost:1935/live/bundle.sdp")
	if err == nil && m.Start() {
		m.WaitForDone()
	}
}
