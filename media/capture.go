package media

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

type Capture struct {
	context *C.AVFormatContext
	frame   *C.AVFrame
	codec   *C.AVCodecContext
	sws     *C.struct_SwsContext
	index   int
	//TODO: add audio capture
}

func NewCapture(driver, device string) (*Capture, error) {
	id := Capture{index: -1}
	id.context = C.avformat_alloc_context()
	if id.context == (*C.AVFormatContext)(null) {
		return nil, fmt.Errorf("allocate output format context failed")
	}
	_driver := C.CString(driver)
	defer C.free(unsafe.Pointer(_driver))
	ifmt := C.av_find_input_format(_driver)
	if ifmt == (*C.AVInputFormat)(null) {
		return nil, fmt.Errorf("cannot find input driver: %s", driver)
	}
	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))
	if C.avformat_open_input(&(id.context), dev, ifmt, (**C.AVDictionary)(null)) < 0 {
		return nil, fmt.Errorf("cannot open device %s", device)
	}
	if C.avformat_find_stream_info(id.context, (**C.AVDictionary)(null)) < 0 {
		return nil, fmt.Errorf("cannot find stream information")
	}
	num := int(id.context.nb_streams)
	streams := (*[1 << 30]*C.AVStream)(unsafe.Pointer(id.context.streams))
	var deCtx *C.AVCodecContext
	for i := 0; i < num; i++ {
		if streams[i].codec.codec_type == C.AVMEDIA_TYPE_VIDEO {
			deCtx = streams[i].codec
			id.index = i
			break
		}
	}
	if id.index == -1 {
		return nil, fmt.Errorf("cannot find video stream")
	}
	codec := C.avcodec_find_decoder(deCtx.codec_id)
	if codec == (*C.AVCodec)(null) {
		return nil, fmt.Errorf("cannot find decode codec")
	}
	id.codec = C.avcodec_alloc_context3(codec)
	/*
		if C.avcodec_copy_context(id.codec, deCtx) != 0 {
			return nil, fmt.Errorf("cannot copy codec context")
		}
	*/
	id.codec = deCtx

	if C.avcodec_open2(id.codec, codec, (**C.struct_AVDictionary)(null)) < 0 {
		return nil, fmt.Errorf("cannot open decode codec")
	}
	id.sws = C.sws_getContext(id.codec.width,
		id.codec.height,
		id.codec.pix_fmt,
		id.codec.width,
		id.codec.height,
		C.AV_PIX_FMT_YUV420P, C.SWS_BILINEAR, (*C.struct_SwsFilter)(null), (*C.struct_SwsFilter)(null), (*C.double)(null))
	id.frame = C.av_frame_alloc()
	return &id, nil
}

func (id *Capture) Close() {
	C.av_frame_free(&(id.frame))
	C.avcodec_close(id.codec)
	C.avformat_close_input(&(id.context))
	C.sws_freeContext(id.sws)
}

func (id *Capture) Read(frame *C.AVFrame) error {
	if frame == (*C.AVFrame)(null) {
		return fmt.Errorf("buffer error")
	}
	pkt := C.AVPacket{}
	C.av_init_packet(&pkt)
	//defer C.av_free_packet(&pkt)
	defer C.av_packet_unref(&pkt)
	if C.av_read_frame(id.context, &pkt) < 0 {
		return fmt.Errorf("read frame error")
	}
	if int(pkt.stream_index) != id.index {
		return fmt.Errorf("not video frame")
	}
	got_frame := C.int(0)
	//if C.avcodec_decode_video2(id.codec, id.frame, &got_frame, &pkt) < 0 {
	if C.avcodec_receive_frame(id.codec, id.frame) < 0 {
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

func (id *Capture) Resolution() (int, int) {
	return int(id.codec.width), int(id.codec.height)
}

func init() {
	C.avdevice_register_all()
}
