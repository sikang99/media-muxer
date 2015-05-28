package mux

/*
#cgo pkg-config: libavdevice libavformat libavcodec libavutil libswscale
#cgo darwin LDFLAGS: -framework cocoa
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/avstring.h>
#include <libswscale/swscale.h>
*/
import "C"
import (
	"fmt"
	"unsafe"
)

type Camera struct {
	context *C.AVFormatContext
	frame   *C.AVFrame
	codec   *C.AVCodecContext
	sws     *C.struct_SwsContext
	index   int
}

func NewCamera(device string) (*Camera, error) {
	camera := Camera{index: -1}
	camera.context = C.avformat_alloc_context()
	if camera.context == (*C.AVFormatContext)(null) {
		return nil, fmt.Errorf("allocate output format context failed")
	}
	driver := C.CString(_DRIVER)
	defer C.free(unsafe.Pointer(driver))
	ifmt := C.av_find_input_format(driver)
	if ifmt == (*C.AVInputFormat)(null) {
		return nil, fmt.Errorf("cannot find input driver: %s", _DRIVER)
	}
	device = _DEVICE_PREFIX + device
	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))
	if C.avformat_open_input(&(camera.context), dev, ifmt, (**C.AVDictionary)(null)) < 0 {
		return nil, fmt.Errorf("cannot open device %s", device)
	}
	if C.avformat_find_stream_info(camera.context, (**C.AVDictionary)(null)) < 0 {
		return nil, fmt.Errorf("cannot find stream information")
	}
	num := int(camera.context.nb_streams)
	streams := (*[1 << 30]*C.AVStream)(unsafe.Pointer(camera.context.streams))
	var deCtx *C.AVCodecContext
	for i := 0; i < num; i++ {
		if streams[i].codec.codec_type == C.AVMEDIA_TYPE_VIDEO {
			deCtx = streams[i].codec
			camera.index = i
			break
		}
	}
	if camera.index == -1 {
		return nil, fmt.Errorf("cannot find video stream")
	}
	codec := C.avcodec_find_decoder(deCtx.codec_id)
	if codec == (*C.AVCodec)(null) {
		return nil, fmt.Errorf("cannot find decode codec")
	}
	camera.codec = C.avcodec_alloc_context3(codec)
	if C.avcodec_copy_context(camera.codec, deCtx) != 0 {
		return nil, fmt.Errorf("cannot copy codec context")
	}
	if C.avcodec_open2(camera.codec, codec, (**C.struct_AVDictionary)(null)) < 0 {
		return nil, fmt.Errorf("cannot open decode codec")
	}
	camera.sws = C.sws_getContext(camera.codec.width,
		camera.codec.height,
		camera.codec.pix_fmt,
		camera.codec.width,
		camera.codec.height,
		C.AV_PIX_FMT_YUV420P, C.SWS_BILINEAR, (*C.struct_SwsFilter)(null), (*C.struct_SwsFilter)(null), (*C.double)(null))
	camera.frame = C.av_frame_alloc()
	return &camera, nil
}

func (id *Camera) Close() {
	C.av_frame_free(&(id.frame))
	C.avcodec_close(id.codec)
	C.avformat_close_input(&(id.context))
	C.sws_freeContext(id.sws)
}

func (id *Camera) Read(frame *C.AVFrame) error {
	if frame == (*C.AVFrame)(null) {
		return fmt.Errorf("buffer error")
	}
	pkt := C.AVPacket{}
	C.av_init_packet(&pkt)
	defer C.av_free_packet(&pkt)
	if C.av_read_frame(id.context, &pkt) < 0 {
		return fmt.Errorf("read frame error")
	}
	if int(pkt.stream_index) != id.index {
		return fmt.Errorf("not video frame")
	}
	got_frame := C.int(0)
	if C.avcodec_decode_video2(id.codec, id.frame, &got_frame, &pkt) < 0 {
		return fmt.Errorf("decode frame error")
	}
	if got_frame != 0 {
		if C.sws_scale(id.sws, (**C.uint8_t)(&(id.frame.data[0])), &id.frame.linesize[0], 0, id.codec.height, &frame.data[0], &frame.linesize[0]) >= 0 {
			return nil
		}
		return fmt.Errorf("scale error")
	}
	return fmt.Errorf("no frame out")
}

func (id *Camera) Resolution() (int, int) {
	return int(id.codec.width), int(id.codec.height)
}

func init() {
	C.avdevice_register_all()
}
