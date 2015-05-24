package mux

/*
  #cgo linux LDFLAGS: -lm -pthread -lz -lbz2 -lfdk-aac -lx264 -lvpx -lopos
  #cgo darwin LDFLAGS: -lz -lbz2 -lfdk-aac -lx264 -lvpx -lopus
  #cgo pkg-config: libavformat
  #cgo pkg-config: libavcodec
  #cgo pkg-config: libavutil
  #cgo pkg-config: libavresample
  #include <libavformat/avformat.h>
  #include <libavcodec/avcodec.h>
  #include <libavutil/avstring.h>
  #include <libavutil/channel_layout.h>
  #include <libavutil/mathematics.h>
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
  static void fill_yuv_image(AVFrame *pict, int frame_index, AVCodecContext *c)
  {
      int x, y, i, ret;

      ret = av_frame_make_writable(pict);
      if (ret < 0)
          exit(1);

      i = frame_index;

      for (y = 0; y < c->height; y++)
          for (x = 0; x < c->width; x++)
              pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

      for (y = 0; y < c->height / 2; y++) {
          for (x = 0; x < c->width / 2; x++) {
              pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
              pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
          }
      }
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
      AVFrame *picture;

      picture = av_frame_alloc();
      if (!picture)
          return NULL;

      picture->format = c->pix_fmt;
      picture->width  = c->width;
      picture->height = c->height;

      if (av_frame_get_buffer(picture, 32) < 0) {
          av_frame_free(&picture);
          return NULL;
      }

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

type Muxer struct {
	context     *C.AVFormatContext
	audioStream Stream
	videoStream Stream
	done        chan bool
}

type Stream struct {
	stream *C.AVStream
	ts     int
}

func NewMuxer(format, uri string) (*Muxer, error) {
	m := Muxer{done: make(chan bool)}
	m.context = C.avformat_alloc_context()
	if m.context == (*C.AVFormatContext)(null) {
		return nil, fmt.Errorf("allocate output format context failed")
	}
	var f *C.char = C.CString(format)
	var u *C.char = C.CString(uri)
	defer C.free(unsafe.Pointer(f))
	defer C.free(unsafe.Pointer(u))
	m.context.oformat = C.av_guess_format(f, u, (*C.char)(null))
	if m.context.oformat == (*C.AVOutputFormat)(null) {
		return nil, fmt.Errorf("output format not supported")
	}
	C.av_strlcpy(&m.context.filename[0], u, C.size_t(unsafe.Sizeof(m.context.filename)))
	return &m, nil
}

func (m *Muxer) AddVideoStream(codecId uint32) bool {
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
	c.width = 640
	c.height = 480
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
	c.sample_rate = 8000
	m.audioStream.stream.time_base = C.AVRational{1, c.sample_rate}
	c.time_base = m.audioStream.stream.time_base
	if m.context.oformat.flags&C.AVFMT_GLOBALHEADER != 0 {
		c.flags |= C.CODEC_FLAG_GLOBAL_HEADER
	}
	if C.avcodec_open2(c, (*C.AVCodec)(null), (**C.AVDictionary)(null)) < 0 {
		return false
	}
	if c.codec.capabilities&C.CODEC_CAP_VARIABLE_FRAME_SIZE != 0 {
		c.frame_size = 10000
	}
	return true
}

func (m *Muxer) writeVideoFrame(frame *C.AVFrame) bool {
	pkt := C.AVPacket{}
	C.av_init_packet(&pkt)
	C.fill_yuv_image(frame, C.int(m.videoStream.ts), m.videoStream.stream.codec)
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
	pkt := C.AVPacket{}
	C.av_init_packet(&pkt)
	C.fill_audio_frame(frame, m.audioStream.stream.codec)
	frame.pts = C.int64_t(m.audioStream.ts)
	m.audioStream.ts += int(m.audioStream.stream.codec.frame_size)
	got_packet := C.int(0)
	if C.avcodec_encode_audio2(m.audioStream.stream.codec, &pkt, frame, &got_packet) < 0 {
		C.av_free_packet(&pkt)
		return false
	}
	if got_packet == 0 {
		return false
	}
	C.av_packet_rescale_ts(&pkt, m.audioStream.stream.codec.time_base, m.audioStream.stream.time_base)
	pkt.stream_index = m.audioStream.stream.index
	return C.av_interleaved_write_frame(m.context, &pkt) == 0
}

func (m *Muxer) Start() bool {
	if !m.AddVideoStream(C.AV_CODEC_ID_H264) {
		return false
	}
	if !m.AddAudioStream(C.AV_CODEC_ID_PCM_MULAW) {
		return false
	}
	if m.context.oformat.flags&C.AVFMT_NOFILE == 0 {
		if C.avio_open(&m.context.pb, &m.context.filename[0], C.AVIO_FLAG_WRITE) < 0 {
			return false
		}
	}
	C.av_dump_format(m.context, 0, &m.context.filename[0], 1)
	if C.avformat_write_header(m.context, (**C.AVDictionary)(null)) < 0 {
		return false
	}
	go m.routine()
	return true
}

func (m *Muxer) Stop() {
	m.done <- true
	C.av_write_trailer(m.context)
	C.avcodec_close(m.videoStream.stream.codec)
	C.avcodec_close(m.audioStream.stream.codec)
	if m.context.oformat.flags&C.AVFMT_NOFILE == 0 {
		C.avio_close(m.context.pb)
	}
	C.avformat_free_context(m.context)
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
	for {
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
}

func init() {
	C.av_register_all()
	C.avformat_network_init()
	// C.av_log_set_level(C.AV_LOG_DEBUG)
}
