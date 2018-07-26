README.md
====

Muxer using libav C library

History
=====

- 2018/07/26 fixed some code to adapt to latest libav library, ie. change functions deprecated
- 2018/07/26 fork from https://github.com/zyxar/media-muxer, which is 3 years old after update.


References
=====
- libav [Deprecated List](https://libav.org/documentation/doxygen/master/deprecated.html)


Changes
=====
```
- for main.go
	- "./media" -> "stoney/media-muxer/media
	- flag.NArg() -> flag.NFlag()

- for media/muxer.go
	- CODEC_CAP_VARIABLE_FRAME_SIZE -> AV_CODEC_CAP_VARIABLE_FRAME_SIZE 
	- CODEC_FLAG_GLOBAL_HEADER -> AV_CODEC_FLAG_GLOBAL_HEADER 

(compiled after above modification)

- for media/{muxer,capture}.go
	- endcode -> receive, decode -> send
	- avcodec_encode_audio2() -> av_receive_frame()
	- avcodec_encode_video2() -> av_receive_frame()
	- avcodec_decode_audio4() -> av_send_frame()	// audio4
	- avcodec_decode_video2() -> av_send_frame()
	- av_free_packet() ->  av_packet_unref()
	- av_register_all() ->  delete (comment out)
	- avcodec_copy_context() -> assign
```


