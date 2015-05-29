package main

import (
	"./mux"

	"flag"
	"fmt"
	"net/url"
)

func main() {
	var driver, device string
	flag.StringVar(&driver, "driver", DRIVER, "set capture driver")
	flag.StringVar(&device, "device", DEVICE, "set capture device")
	flag.Parse()
	if flag.NArg() == 0 {
		fmt.Println("no thing to mux")
		flag.Usage()
		return
	}
	uri, err := url.Parse(flag.Arg(0))
	if err != nil {
		fmt.Println(err)
		return
	}
	var format string
	if uri.Scheme == "rtsp" {
		format = "rtsp"
	}
	media := mux.MediaSource{mux.NewVideoSource(driver, device), &(mux.AudioSource{})}
	m, err := mux.NewMuxer(&media, format, flag.Arg(0))
	if err == nil {
		if m.Start() {
			m.WaitForDone()
		}
		m.Close()
	} else {
		fmt.Println(err)
	}
}

func init() {
	// C.av_log_set_level(C.AV_LOG_DEBUG)
}
