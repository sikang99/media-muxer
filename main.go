package main

import (
	"./mux"

	"flag"
	"fmt"
	"net/url"
)

func main() {
	flag.Parse()
	if flag.NArg() == 0 {
		fmt.Println("no thing to mux")
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
	m, err := mux.NewMuxer(format, flag.Arg(0))
	if err == nil {
		if m.Start() {
			m.WaitForDone()
		}
		m.Close()
	} else {
		fmt.Println(err)
	}
}
