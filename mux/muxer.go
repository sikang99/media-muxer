package mux

/*
  #cgo linux LDFLAGS: -lm
  #cgo pkg-config: libavformat libavcodec libavutil
  #include <libavformat/avformat.h>
  #include <libavcodec/avcodec.h>
  #include <libavutil/audio_fifo.h>
  #include <libavutil/avstring.h>
  #include <libavutil/channel_layout.h>
  #include <libavutil/imgutils.h>
  #include <libavutil/mathematics.h>
  inline static int min(int a, int b) {
    return ((a) > (b) ? (b) : (a));
  }
  static int check_sample_fmt(AVCodec *codec, enum AVSampleFormat sample_fmt)
  {
      const enum AVSampleFormat *p = codec->sample_fmts;

      while (*p != AV_SAMPLE_FMT_NONE) {
          if (*p == sample_fmt)
              return 1;
          p++;
      }
      return 0;
  }
  static void fill_audio_frame(AVFrame* frame, AVCodecContext *c)
  {
    float t = 0, tincr = 2 * M_PI * 110.0 / c->sample_rate, tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;
    int j, i, v;
    int16_t *q = (int16_t*)frame->data[0];
    for (j = 0; j < frame->nb_samples; j++) {
        v = (int)(sin(t) * 10000);
        for (i = 0; i < c->channels; i++)
            *q++ = v;
        t     += tincr;
        tincr += tincr2;
    }
  }
  static AVFrame* alloc_video_frame(AVCodecContext* c)
  {
      AVFrame* picture = av_frame_alloc();
      if (!picture)
          return NULL;
      if (!c)
          return picture;
      picture->format = c->pix_fmt;
      picture->width  = c->width;
      picture->height = c->height;
      av_image_alloc(picture->data, picture->linesize, c->width, c->height, c->pix_fmt, 32);
      return picture;
  }

  static AVFrame* alloc_audio_frame(AVCodecContext* c)
  {
      int buffer_size;

      AVFrame* frame = av_frame_alloc();
      if (!frame) {
          av_log(NULL, AV_LOG_ERROR, "allocate audio frame failed\n");
          return NULL;
      }

      frame->nb_samples     = c->frame_size;
      frame->format         = c->sample_fmt;
      frame->channel_layout = c->channel_layout;
      frame->sample_rate    = c->sample_rate;

      if (frame->nb_samples) {
        if (av_frame_get_buffer(frame, 0) < 0) {
          return NULL;
        }
      }
      return frame;
  }
*/
import "C"
import (
	"fmt"
	"time"
	"unsafe"
)

var (
	null = unsafe.Pointer(uintptr(0))
)

type reclaimer func()

type Muxer struct {
	done chan bool
	loop bool
	recl []reclaimer
	// output
	context     *C.AVFormatContext
	audioStream Stream
	videoStream Stream
	fifo        *C.AVAudioFifo
	// input
	capture *Capture
	// display
	display *Display
}

type Stream struct {
	stream *C.AVStream
	ts     int
}

type VideoSource struct {
	driver, device string
}

func NewVideoSource(driver, device string) *VideoSource {
	return &VideoSource{driver, device}
}

type AudioSource struct{}

type MediaSource struct {
	Video *VideoSource
	Audio *AudioSource
}

func NewMuxer(source *MediaSource, format, uri string) (*Muxer, error) {
	m := Muxer{done: make(chan bool), recl: make([]reclaimer, 0, 8)}
	m.context = C.avformat_alloc_context()
	if m.context == (*C.AVFormatContext)(null) {
		return nil, fmt.Errorf("allocate output format context failed")
	}
	m.recl = append(m.recl, func() {
		C.avformat_free_context(m.context)
	})
	var f *C.char = C.CString(format)
	var u *C.char = C.CString(uri)
	defer C.free(unsafe.Pointer(f))
	defer C.free(unsafe.Pointer(u))
	m.context.oformat = C.av_guess_format(f, u, (*C.char)(null))
	if m.context.oformat == (*C.AVOutputFormat)(null) {
		return nil, fmt.Errorf("output format not supported")
	}
	C.av_strlcpy(&m.context.filename[0], u, C.size_t(unsafe.Sizeof(m.context.filename)))
	var err error
	if m.capture, err = NewCapture(source.Video.driver, source.Video.device); err != nil {
		return nil, err
	}
	return &m, nil
}

func (m *Muxer) EnableDisplay() {
	if m.display != nil {
		return
	}
	w, h := m.capture.Resolution()
	m.display, _ = NewDisplay("Camera", w, h)
}

func (m *Muxer) DisableDisplay() {
	if m.display != nil {
		m.display.Close()
		m.display = nil
	}
}

func (m *Muxer) AddVideoStream(codecId uint32, width, height int) bool {
	codec := C.avcodec_find_encoder(codecId)
	if codec == (*C.AVCodec)(null) {
		return false
	}
	m.context.oformat.video_codec = codecId
	stream := C.avformat_new_stream(m.context, codec)
	if stream == (*C.AVStream)(null) {
		return false
	}
	m.videoStream = Stream{stream, 0}
	c := m.videoStream.stream.codec
	c.codec_id = codecId
	c.codec_type = C.AVMEDIA_TYPE_VIDEO
	c.bit_rate = 400000
	c.width = C.int(width)
	c.height = C.int(height)
	m.videoStream.stream.time_base = C.AVRational{1, 30}
	c.time_base = m.videoStream.stream.time_base
	c.gop_size = 12
	c.pix_fmt = C.AV_PIX_FMT_YUV420P
	if m.context.oformat.flags&C.AVFMT_GLOBALHEADER != 0 {
		c.flags |= C.CODEC_FLAG_GLOBAL_HEADER
	}
	if C.avcodec_open2(c, (*C.AVCodec)(null), (**C.AVDictionary)(null)) < 0 {
		return false
	}
	m.recl = append(m.recl, func() {
		C.avcodec_close(c)
	})
	return true
}

func (m *Muxer) AddAudioStream(codecId uint32) bool {
	codec := C.avcodec_find_encoder(codecId)
	if codec == (*C.AVCodec)(null) {
		return false
	}
	m.context.oformat.audio_codec = codecId
	stream := C.avformat_new_stream(m.context, codec)
	if stream == (*C.AVStream)(null) {
		return false
	}
	m.audioStream = Stream{stream, 0}
	c := m.audioStream.stream.codec
	c.bit_rate = 48000
	c.sample_fmt = C.AV_SAMPLE_FMT_S16
	if C.check_sample_fmt(codec, c.sample_fmt) == 0 {
		return false
	}
	c.channels = 1
	c.channel_layout = C.uint64_t(C.av_get_default_channel_layout(c.channels))
	c.sample_rate = 44100
	m.audioStream.stream.time_base = C.AVRational{1, c.sample_rate}
	c.time_base = m.audioStream.stream.time_base
	if m.context.oformat.flags&C.AVFMT_GLOBALHEADER != 0 {
		c.flags |= C.CODEC_FLAG_GLOBAL_HEADER
	}
	if codecId == C.AV_CODEC_ID_AAC {
		m.fifo = C.av_audio_fifo_alloc(c.sample_fmt, c.channels, 1)
		if m.fifo == (*C.AVAudioFifo)(null) {
			return false
		}
		m.recl = append(m.recl, func() {
			C.av_audio_fifo_free(m.fifo)
		})
	}
	if C.avcodec_open2(c, (*C.AVCodec)(null), (**C.AVDictionary)(null)) < 0 {
		return false
	}
	m.recl = append(m.recl, func() {
		C.avcodec_close(c)
	})
	if c.codec.capabilities&C.CODEC_CAP_VARIABLE_FRAME_SIZE != 0 {
		c.frame_size = 10000
	}
	return true
}

func (m *Muxer) writeVideoFrame(frame *C.AVFrame) bool {
	if m.capture.Read(frame) != nil {
		return false
	}
	if m.display != nil {
		m.display.Render(frame)
	}
	pkt := C.AVPacket{}
	C.av_init_packet(&pkt)
	frame.pts = C.int64_t(m.videoStream.ts)
	m.videoStream.ts++
	got_packet := C.int(0)
	if C.avcodec_encode_video2(m.videoStream.stream.codec, &pkt, frame, &got_packet) < 0 {
		C.av_free_packet(&pkt)
		return false
	}
	if got_packet == 0 {
		return false
	}
	C.av_packet_rescale_ts(&pkt, m.videoStream.stream.codec.time_base, m.videoStream.stream.time_base)
	pkt.stream_index = m.videoStream.stream.index
	return C.av_interleaved_write_frame(m.context, &pkt) == 0
}

func (m *Muxer) writeAudioFrame(frame *C.AVFrame) bool {
	for C.av_audio_fifo_size(m.fifo) < 1024 { // generate & store in fifo
		C.fill_audio_frame(frame, m.audioStream.stream.codec)
		frame_size := frame.nb_samples
		if C.av_audio_fifo_realloc(m.fifo, C.av_audio_fifo_size(m.fifo)+frame_size) < 0 {
			return false
		}
		if C.av_audio_fifo_write(m.fifo, (*unsafe.Pointer)(unsafe.Pointer(&frame.data[0])), frame_size) < frame_size {
			return false
		}
	}
	got_packet := C.int(0)
	for C.av_audio_fifo_size(m.fifo) >= 1024 { // read & encode & write
		frame_size := C.min(C.av_audio_fifo_size(m.fifo), m.audioStream.stream.codec.frame_size)
		output_frame := C.alloc_audio_frame(m.audioStream.stream.codec)
		if C.av_audio_fifo_read(m.fifo, (*unsafe.Pointer)(unsafe.Pointer(&output_frame.data[0])), frame_size) < frame_size {
			C.av_frame_free(&output_frame)
			return false
		}
		pkt := C.AVPacket{}
		C.av_init_packet(&pkt)
		output_frame.pts = C.int64_t(m.audioStream.ts)
		m.audioStream.ts += int(m.audioStream.stream.codec.frame_size)
		if C.avcodec_encode_audio2(m.audioStream.stream.codec, &pkt, frame, &got_packet) < 0 {
			C.av_free_packet(&pkt)
			return false
		}
		if got_packet == 0 {
			continue
		}
		C.av_packet_rescale_ts(&pkt, m.audioStream.stream.codec.time_base, m.audioStream.stream.time_base)
		pkt.stream_index = m.audioStream.stream.index
		if C.av_interleaved_write_frame(m.context, &pkt) < 0 {
			return false
		}
	}
	return true
}

func (m *Muxer) Open() bool {
	w, h := m.capture.Resolution()
	if !m.AddVideoStream(C.AV_CODEC_ID_H264, w, h) {
		return false
	}
	if !m.AddAudioStream(C.AV_CODEC_ID_AAC) {
		return false
	}
	if m.context.oformat.flags&C.AVFMT_NOFILE == 0 {
		if C.avio_open(&m.context.pb, &m.context.filename[0], C.AVIO_FLAG_WRITE) < 0 {
			return false
		}
		m.recl = append(m.recl, func() {
			C.avio_close(m.context.pb)
		})
	}
	C.av_dump_format(m.context, 0, &m.context.filename[0], 1)
	if C.avformat_write_header(m.context, (**C.AVDictionary)(null)) < 0 {
		return false
	}
	m.recl = append(m.recl, func() {
		C.av_write_trailer(m.context)
	})
	m.loop = true
	go m.routine()
	if m.display != nil {
		m.display.Open()
	}
	return true
}

func (m *Muxer) Close() {
	if m.display != nil {
		m.display.Close()
	}
	m.capture.Close()
	m.loop = false
	for i := len(m.recl) - 1; i >= 0; i-- { // reclaim resources
		m.recl[i]()
	}
}

func (m *Muxer) WaitForDone() {
	<-m.done
}

func (m *Muxer) CheckForDone() bool {
	select {
	case <-m.done:
		return true
	default:
		return false
	}
}

func (m *Muxer) routine() {
	vFrame := C.alloc_video_frame(m.videoStream.stream.codec)
	if vFrame == (*C.AVFrame)(null) {
		m.done <- true
		return
	}
	aFrame := C.alloc_audio_frame(m.audioStream.stream.codec)
	if aFrame == (*C.AVFrame)(null) {
		m.done <- true
		return
	}
	for m.loop {
		if C.av_compare_ts(C.int64_t(m.videoStream.ts), m.videoStream.stream.codec.time_base,
			C.int64_t(m.audioStream.ts), m.audioStream.stream.codec.time_base) <= 0 {
			m.writeVideoFrame(vFrame)
		} else {
			m.writeAudioFrame(aFrame)
		}
		time.Sleep(time.Millisecond * 30)
	}
	if vFrame != (*C.AVFrame)(null) {
		C.av_frame_free(&vFrame)
	}
	if aFrame != (*C.AVFrame)(null) {
		C.av_frame_free(&aFrame)
	}
	m.done <- true
}

func init() {
	C.av_register_all()
	C.avformat_network_init()
}
