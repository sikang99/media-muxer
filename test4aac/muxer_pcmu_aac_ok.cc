#include <boost/thread.hpp>
#include <string>
#include <iostream>
#include <fstream>
#include <stdio.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>

#include "libavformat/avio.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

#include "libswresample/swresample.h"
}

/** The output bit rate in kbit/s */
#define OUTPUT_BIT_RATE 96000
/** The number of output channels */
#define OUTPUT_CHANNELS 2

#define PCMU_INDEX  59

#define AAC_INDEX  238

static int g_index = 0;
static int g_index_aac = 0;
static int g_pcmu_index = 0;

using namespace std;

class AudioData {
  public:
    AudioData(int size);
    virtual ~AudioData();
    char* getAudioData();
    int getAudioSize();

  private:
    char *data;
    int size;
};
char* AudioData::getAudioData() {
  return data;
}
int AudioData::getAudioSize() {
  return size;
}

AudioData::AudioData(int len) {
  data = new char[len];
  size = len;
}

AudioData::~AudioData() {
  if (data) {
    delete(data);
    data = NULL;
  }
}

vector<AudioData*> pcmu_audios;
AudioData* audioPCMUData;

AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
AVCodecContext *input_codec_context = NULL, *output_codec_context = NULL;
SwrContext *resample_context = NULL;
AVAudioFifo *fifo = NULL;
int ret = AVERROR_EXIT;

/* check that a given sample format is supported by the encoder */
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

static void fill_yuv_image(AVFrame *pict, int frame_index, int width, int height)
{
    int x, y, i, ret;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally;
     * make sure we do not overwrite it here
     */
    ret = av_frame_make_writable(pict);
    if (ret < 0)
        exit(1);

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
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

    /* allocate the buffers for the frame data */
    if (av_frame_get_buffer(picture, 32) < 0) {
        av_frame_free(&picture);
        return NULL;
    }

    return picture;
}

/**
 * Convert an error code into a text message.
 * @param error Error code to be converted
 * @return Corresponding error text (not thread-safe)
 */
static const char *get_error_text(const int error)
{
    static char error_buffer[255];
    av_strerror(error, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

/** Open an input file and the required decoder. */
static int open_input_file(const char* filename,
                           AVFormatContext **input_format_context,
                           AVCodecContext **input_codec_context)
{
    AVCodec *input_codec;
    int error;

// only for testing, let ffmpeg engin to initilizaiton AVContext by analzing the supplied input pcmu file
#if 1
    /** Open the input file to read from it. */
    if ((error = avformat_open_input(input_format_context, filename, NULL,
                                     NULL)) < 0) {
        fprintf(stderr, "Could not open input file '%s' (error '%s')\n",
                filename, get_error_text(error));
        *input_format_context = NULL;
        return error;
    }

    /** Get information on the input file (number of streams etc.). */
    if ((error = avformat_find_stream_info(*input_format_context, NULL)) < 0) {
        fprintf(stderr, "Could not open find stream info (error '%s')\n",
                get_error_text(error));
        avformat_close_input(input_format_context);
        return error;
    }

    /** Make sure that there is only one stream in the input file. */
    if ((*input_format_context)->nb_streams != 1) {
        fprintf(stderr, "Expected one audio input stream, but found %d\n",
                (*input_format_context)->nb_streams);
        avformat_close_input(input_format_context);
        return AVERROR_EXIT;
    }
#endif
    /** Find a decoder for the audio stream. */
    // for pcm_mulaw, we can set it to AV_CODEC_ID_PCM_MULAW directly here
    if (!(input_codec = avcodec_find_decoder((*input_format_context)->streams[0]->codec->codec_id))) {
//    if (!(input_codec = avcodec_find_decoder(AV_CODEC_ID_PCM_MULAW)) {
        fprintf(stderr, "Could not find input codec\n");
        avformat_close_input(input_format_context);
        return AVERROR_EXIT;
    }

    /** Open the decoder for the audio stream to use it later. */
    if ((error = avcodec_open2((*input_format_context)->streams[0]->codec,
                               input_codec, NULL)) < 0) {
        fprintf(stderr, "Could not open input codec (error '%s')\n",
                get_error_text(error));
        avformat_close_input(input_format_context);
        return error;
    }

    /** Save the decoder context for easier access later. */
    *input_codec_context = (*input_format_context)->streams[0]->codec;

    return 0;
}

/**
 * Open an output file and the required encoder.
 * Also set some basic encoder parameters.
 * Some of these parameters are based on the input file's parameters.
 */
static int open_output_file(const char *filename,
                            AVCodecContext *input_codec_context,
                            AVFormatContext **output_format_context,
                            AVCodecContext **output_codec_context)
{
    AVIOContext *output_io_context = NULL;
    AVStream *stream               = NULL;
    AVCodec *output_codec          = NULL;
    int error;

    /** Open the output file to write to it. */
    if ((error = avio_open(&output_io_context, filename,
                           AVIO_FLAG_WRITE)) < 0) {
        fprintf(stderr, "Could not open output file '%s' (error '%s')\n",
                filename, get_error_text(error));
        return error;
    }

    /** Create a new format context for the output container format. */
    if (!(*output_format_context = avformat_alloc_context())) {
        fprintf(stderr, "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }

    /** Associate the output file (pointer) with the container format context. */
    (*output_format_context)->pb = output_io_context;

    /** Guess the desired container format based on the file extension. */
    if (!((*output_format_context)->oformat = av_guess_format(NULL, filename,
                                                              NULL))) {
        fprintf(stderr, "Could not find output file format\n");
        goto cleanup;
    }

    av_strlcpy((*output_format_context)->filename, filename,
               sizeof((*output_format_context)->filename));

    /** Find the encoder to be used by its name. */
    if (!(output_codec = avcodec_find_encoder(AV_CODEC_ID_AAC))) {
        fprintf(stderr, "Could not find an AAC encoder.\n");
        goto cleanup;
    }

    /** Create a new audio stream in the output file container. */
    if (!(stream = avformat_new_stream(*output_format_context, output_codec))) {
        fprintf(stderr, "Could not create new stream\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    /** Save the encoder context for easier access later. */
    *output_codec_context = stream->codec;

    /**
     * Set the basic encoder parameters.
     * The input file's sample rate is used to avoid a sample rate conversion.
     */
    (*output_codec_context)->channels       = OUTPUT_CHANNELS;
    (*output_codec_context)->channel_layout = av_get_default_channel_layout(OUTPUT_CHANNELS);
    (*output_codec_context)->sample_rate    = input_codec_context->sample_rate;
    (*output_codec_context)->sample_fmt     = output_codec->sample_fmts[0];
    (*output_codec_context)->bit_rate       = OUTPUT_BIT_RATE;

    /** Allow the use of the experimental AAC encoder */
    (*output_codec_context)->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    /** Set the sample rate for the container. */
    stream->time_base.den = 44100;//input_codec_context->sample_rate;
    stream->time_base.num = 1;

    /**
     * Some container formats (like MP4) require global headers to be present
     * Mark the encoder so that it behaves accordingly.
     */
    if ((*output_format_context)->oformat->flags & AVFMT_GLOBALHEADER)
        (*output_codec_context)->flags |= CODEC_FLAG_GLOBAL_HEADER;

    /** Open the encoder for the audio stream to use it later. */
    if ((error = avcodec_open2(*output_codec_context, output_codec, NULL)) < 0) {
        fprintf(stderr, "Could not open output codec (error '%s')\n",
                get_error_text(error));
        goto cleanup;
    }

    return 0;

cleanup:
    avio_closep(&(*output_format_context)->pb);
    avformat_free_context(*output_format_context);
    *output_format_context = NULL;
    return error < 0 ? error : AVERROR_EXIT;
}

/** Initialize one data packet for reading or writing. */
static void init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    /** Set the packet data and size so that it is recognized as being empty. */
    packet->data = NULL;
    packet->size = 0;
}

/** Initialize one audio frame for reading from the input file */
static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate input frame\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Initialize the audio resampler based on the input and output codec settings.
 * If the input and output sample formats differ, a conversion is required
 * libswresample takes care of this, but requires initialization.
 */
static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context)
{
        int error;

        /**
         * Create a resampler context for the conversion.
         * Set the conversion parameters.
         * Default channel layouts based on the number of channels
         * are assumed for simplicity (they are sometimes not detected
         * properly by the demuxer and/or decoder).
         */
        *resample_context = swr_alloc_set_opts(NULL,
                                              av_get_default_channel_layout(output_codec_context->channels),
                                              output_codec_context->sample_fmt,
                                              output_codec_context->sample_rate,
                                              av_get_default_channel_layout(input_codec_context->channels),
                                              input_codec_context->sample_fmt,
                                              input_codec_context->sample_rate,
                                              0, NULL);
        if (!*resample_context) {
            fprintf(stderr, "Could not allocate resample context\n");
            return AVERROR(ENOMEM);
        }
        /**
        * Perform a sanity check so that the number of converted samples is
        * not greater than the number of samples to be converted.
        * If the sample rates differ, this case has to be handled differently
        */
        av_assert0(output_codec_context->sample_rate == input_codec_context->sample_rate);

        /** Open the resampler with the specified parameters. */
        if ((error = swr_init(*resample_context)) < 0) {
            fprintf(stderr, "Could not open resample context\n");
            swr_free(resample_context);
            return error;
        }
    return 0;
}

/** Initialize a FIFO buffer for the audio samples to be encoded. */
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    /** Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->channels, 1))) {
        fprintf(stderr, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/** Write the header of the output file container. */
static int write_output_file_header(AVFormatContext *output_format_context)
{
    int error;
    if ((error = avformat_write_header(output_format_context, NULL)) < 0) {
        fprintf(stderr, "Could not write output file header (error '%s')\n",
                get_error_text(error));
        return error;
    }
    return 0;
}

/** Decode one audio frame from the input file. */
static int decode_audio_frame(AVFrame *frame,
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished)
{
    /** Packet used for temporary storage. */
    AVPacket input_packet;
    int error;
    init_packet(&input_packet);

#if 1
// do only once for initializing the input_format_context which can be used to decode our PCMU buffer
if (g_pcmu_index == 0) {
    /** Read one audio frame from the input file into a temporary packet. */
    if ((error = av_read_frame(input_format_context, &input_packet)) < 0) {
        /** If we are at the end of the file, flush the decoder below. */
        if (error == AVERROR_EOF)
            *finished = 1;
        else {
            fprintf(stderr, "Could not read frame (error '%s')\n",
                    get_error_text(error));
            return error;
        }
    }
}

#endif
   if (g_pcmu_index == PCMU_INDEX) {
      g_pcmu_index = 1;
    }

  audioPCMUData = pcmu_audios[g_pcmu_index];

  {
    int size = audioPCMUData->getAudioSize();
    printf("prepared packet size = %d\n", size);
    input_packet.data     = (unsigned char*)audioPCMUData->getAudioData();//buf->data;
    input_packet.size     = size;
  }
  g_pcmu_index++;

//  pcmu_audios.pop_back();

    /**
     * Decode the audio frame stored in the temporary packet.
     * The input audio stream decoder is used to do this.
     * If we are at the end of the file, pass an empty packet to the decoder
     * to flush it.
     */
    if ((error = avcodec_decode_audio4(input_codec_context, frame,
                                       data_present, &input_packet)) < 0) {
        fprintf(stderr, "Could not decode frame (error '%s')\n",
                get_error_text(error));
        av_free_packet(&input_packet);
        return error;
    }

    /**
     * If the decoder has not been flushed completely, we are not finished,
     * so that this function has to be called again.
     */
    if (*finished && *data_present)
        *finished = 0;
    av_free_packet(&input_packet);
    return 0;
}

/**
 * Initialize a temporary storage for the specified number of audio samples.
 * The conversion requires temporary storage due to the different format.
 * The number of audio samples to be allocated is specified in frame_size.
 */
static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size)
{
    int error;

    /**
     * Allocate as many pointers as there are audio channels.
     * Each pointer will later point to the audio samples of the corresponding
     * channels (although it may be NULL for interleaved formats).
     */
    if (!(*converted_input_samples = (uint8_t **)calloc(output_codec_context->channels,
                                            sizeof(**converted_input_samples)))) {
        fprintf(stderr, "Could not allocate converted input sample pointers\n");
        return AVERROR(ENOMEM);
    }

    /**
     * Allocate memory for the samples of all channels in one consecutive
     * block for convenience.
     */
    if ((error = av_samples_alloc(*converted_input_samples, NULL,
                                  output_codec_context->channels,
                                  frame_size,
                                  output_codec_context->sample_fmt, 0)) < 0) {
        fprintf(stderr,
                "Could not allocate converted input samples (error '%s')\n",
                get_error_text(error));
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);
        return error;
    }
    return 0;
}

/**
 * Convert the input audio samples into the output sample format.
 * The conversion happens on a per-frame basis, the size of which is specified
 * by frame_size.
 */
static int convert_samples(const uint8_t **input_data,
                           uint8_t **converted_data, const int frame_size,
                           SwrContext *resample_context)
{
    int error;

    /** Convert the samples using the resampler. */
    if ((error = swr_convert(resample_context,
                             converted_data, frame_size,
                             input_data    , frame_size)) < 0) {
        fprintf(stderr, "Could not convert input samples (error '%s')\n",
                get_error_text(error));
        return error;
    }

    return 0;
}

/** Add converted input audio samples to the FIFO buffer for later processing. */
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error;

    /**
     * Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples.
     */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        fprintf(stderr, "Could not reallocate FIFO\n");
        return error;
    }

    /** Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        fprintf(stderr, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}

/**
 * Read one audio frame from the input file, decodes, converts and stores
 * it in the FIFO buffer.
 */
static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         AVCodecContext *output_codec_context,
                                         SwrContext *resampler_context,
                                         int *finished)
{
    /** Temporary storage of the input samples of the frame read from the file. */
    AVFrame *input_frame = NULL;
    /** Temporary storage for the converted input samples. */
    uint8_t **converted_input_samples = NULL;
    int data_present;
    int ret = AVERROR_EXIT;

    /** Initialize temporary storage for one input frame. */
    if (init_input_frame(&input_frame))
        goto cleanup;
    /** Decode one frame worth of audio samples. */
    if (decode_audio_frame(input_frame, input_format_context,
                           input_codec_context, &data_present, finished))
        goto cleanup;
    /**
     * If we are at the end of the file and there are no more samples
     * in the decoder which are delayed, we are actually finished.
     * This must not be treated as an error.
     */
    if (*finished && !data_present) {
        ret = 0;
        goto cleanup;
    }
    /** If there is decoded data, convert and store it */
    if (data_present) {
        /** Initialize the temporary storage for the converted input samples. */
        if (init_converted_samples(&converted_input_samples, output_codec_context,
                                   input_frame->nb_samples))
            goto cleanup;

        /**
         * Convert the input samples to the desired output sample format.
         * This requires a temporary storage provided by converted_input_samples.
         */
        if (convert_samples((const uint8_t**)input_frame->extended_data, converted_input_samples,
                            input_frame->nb_samples, resampler_context))
            goto cleanup;

        /** Add the converted input samples to the FIFO buffer for later processing. */
        if (add_samples_to_fifo(fifo, converted_input_samples,
                                input_frame->nb_samples))
            goto cleanup;
        ret = 0;
    }
    ret = 0;

cleanup:
    if (converted_input_samples) {
        av_freep(&converted_input_samples[0]);
        free(converted_input_samples);
    }
    av_frame_free(&input_frame);

    return ret;
}

/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;

    /** Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /**
     * Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity.
     */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /**
     * Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified.
     */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        fprintf(stderr, "Could allocate output frame samples (error '%s')\n",
                get_error_text(error));
        av_frame_free(frame);
        return error;
    }

    return 0;
}

/** Global timestamp for the audio frames */
static int64_t pts = 0;

/** Encode one frame worth of audio to the output file. */
static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present,
                              AVStream *mAudioStream,
                              AVFormatContext* mContext)
{
    /** Packet used for temporary storage. */
    AVPacket output_packet;
    int error;
    init_packet(&output_packet);

    /** Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = pts;
        pts += frame->nb_samples;
    }

    /**
     * Encode the audio frame and store it in the temporary packet.
     * The output audio stream encoder is used to do this.
     */
    if ((error = avcodec_encode_audio2(output_codec_context, &output_packet,
                                       frame, data_present)) < 0) {
        fprintf(stderr, "Could not encode frame (error '%s')\n",
                get_error_text(error));
        av_free_packet(&output_packet);
        return error;
    }

    /** Write one audio frame from the temporary packet to the output file. */
    if (*data_present) {
#if 0
        if ((error = av_write_frame(output_format_context, &output_packet)) < 0) {
            fprintf(stderr, "Could not write frame (error '%s')\n",
                    get_error_text(error));
            av_free_packet(&output_packet);
            return error;
        }
#else
        av_packet_rescale_ts(&output_packet, mAudioStream->codec->time_base, mAudioStream->time_base);
        output_packet.stream_index = mAudioStream->index;
        error = av_interleaved_write_frame(mContext, &output_packet);

        if (error < 0) {
          printf("av_interleaved_write_frame failed!!\n");
//          return error;
        }
#endif
        av_free_packet(&output_packet);
    }

    return 0;
}

/**
 * Load one audio frame from the FIFO buffer, encode and write it to the
 * output file.
 */
static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context,
                                 AVStream *mAudioStream,
                                 AVFormatContext* mContext)
{
    /** Temporary storage of the output samples of the frame written to the file. */
    AVFrame *output_frame;
    /**
     * Use the maximum number of possible samples per frame.
     * If there is less than the maximum possible frame size in the FIFO
     * buffer use this number. Otherwise, use the maximum possible frame size
     */
    const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                                 output_codec_context->frame_size);
    int data_written;

    /** Initialize temporary storage for one output frame. */
    if (init_output_frame(&output_frame, output_codec_context, frame_size))
        return AVERROR_EXIT;

    /**
     * Read as many samples from the FIFO buffer as required to fill the frame.
     * The samples are stored in the frame temporarily.
     */
    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
        fprintf(stderr, "Could not read data from FIFO\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /** Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, output_format_context,
                           output_codec_context, &data_written, mAudioStream, mContext)) {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

/** Write the trailer of the output file container. */
static int write_output_file_trailer(AVFormatContext *output_format_context)
{
    int error;
    if ((error = av_write_trailer(output_format_context)) < 0) {
        fprintf(stderr, "Could not write output file trailer (error '%s')\n",
                get_error_text(error));
        return error;
    }
    return 0;
}


static AVFrame* alloc_audio_frame(AVCodecContext* c, uint8_t** buffer)
{
    int buffer_size;

    /* frame containing input raw audio */
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "allocate audio frame failed\n");
        return NULL;
    }

    frame->nb_samples     = c->frame_size;
    frame->format         = c->sample_fmt;
    frame->channel_layout = c->channel_layout;

    /* the codec gives us the frame size, in samples,
     * we calculate the size of the samples buffer in bytes */
    av_log(NULL, AV_LOG_DEBUG, "alloc_audio_frame - channels:%d, nb_samples:%d, format:%s\n", c->channels, c->frame_size, av_get_sample_fmt_name(c->sample_fmt));
    buffer_size = av_samples_get_buffer_size(NULL, c->channels, c->frame_size, c->sample_fmt, 0);
    uint8_t* samples = reinterpret_cast<uint8_t*>(av_malloc(buffer_size));
    if (!samples) {
        av_log(NULL, AV_LOG_ERROR, "allocate %d bytes for samples buffer failed\n", buffer_size);
        return NULL;
    }
    /* setup the data pointers in the AVFrame */
    if (avcodec_fill_audio_frame(frame, c->channels, c->sample_fmt, samples, buffer_size, 0) < 0) {
        av_log(NULL, AV_LOG_ERROR, "setup audio frame failed\n");
        return NULL;
    }
    *buffer = samples;
    return frame;
}


class Muxer {
public:
    Muxer(const char*, const std::string&);
    virtual ~Muxer();
    bool start();
    void stop();
private:
    AVFormatContext* mContext;
    AVStream* mAudioStream;
    AVStream* mVideoStream;
    bool mMuxing;
    bool mHasVideo;
    bool mHasAudio;
    std::vector<AudioData*> audios;
    AudioData* audioData;
    int index;
    int output_frame_size;
    int finished;

    bool addAudioStream(enum AVCodecID);
    bool addVideoStream(enum AVCodecID);
    int writeAudioFrame(AVFrame*, int, uint8_t*);
    int writeVideoFrame(AVFrame*, int);
    void loop();
    boost::thread mThread;
};

Muxer::Muxer(const char* fmt, const std::string& uri)
    : mContext (NULL)
    , mAudioStream (NULL)
    , mVideoStream (NULL)
    , mMuxing (false)
    , mHasVideo (false)
    , mHasAudio (false)
    , index (0)
    , output_frame_size (0)
    , finished (0)
{
#ifdef FAKE_AAC
  char file[32];
  for (int i = 0; i < AAC_INDEX; i++) {
    sprintf(file, "dump_files/dump_aac_%d", i);
    ifstream in;
    in.open(file, ios_base::binary);
    in.seekg(0,ios::end);
    long size = in.tellg();
    printf("%s, frame %d, size is %d\n", file, i, (int)size);
    ifstream fin;
    fin.open(file, ios_base::binary);
    //    char *tmp = new char[size];
    audioData = new AudioData((int)size);
    fin.read(audioData->getAudioData(), size);
    fin.close();
    audios.push_back(audioData);
    in.close();
  }
#endif


    mContext = avformat_alloc_context();
    if (!mContext) {
        av_log(NULL, AV_LOG_ERROR, "allocate output format context failed\n");
        return;
    }

    mContext->oformat = av_guess_format(fmt, uri.c_str(), NULL);
    if (!mContext->oformat) {
        av_log(NULL, AV_LOG_ERROR, "output format not supported\n");
        return;
    }

    av_strlcpy(mContext->filename, uri.c_str(), sizeof(mContext->filename));
}

Muxer::~Muxer()
{
    if (mMuxing)
        stop();
}

static int packet_alloc(AVBufferRef **buf, int size)
{
    int ret;
      if ((unsigned)size >= (unsigned)size + FF_INPUT_BUFFER_PADDING_SIZE)
            return AVERROR(EINVAL);

        ret = av_buffer_realloc(buf, size + FF_INPUT_BUFFER_PADDING_SIZE);
          if (ret < 0)
                return ret;

            memset((*buf)->data + size, 0, FF_INPUT_BUFFER_PADDING_SIZE);

              return 0;
}

void Muxer::stop()
{
    mMuxing = false;
    mThread.join();

    av_write_trailer(mContext);
    avcodec_close(mAudioStream->codec);
    if (!(mContext->oformat->flags & AVFMT_NOFILE))
        avio_close(mContext->pb);
    avformat_free_context(mContext);
}

bool Muxer::start() {
#ifdef HAS_VIDEO
    if (addVideoStream(AV_CODEC_ID_H264))
        mHasVideo = true;
#endif
#ifdef AUDIO_PCMU
    if (addAudioStream(AV_CODEC_ID_PCM_MULAW))
#else
    if (addAudioStream(AV_CODEC_ID_AAC))
#endif
        mHasAudio = true;
    if (mHasAudio || mHasVideo) {
        if (!(mContext->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&mContext->pb, mContext->filename, AVIO_FLAG_WRITE) < 0) {
                av_log(NULL, AV_LOG_ERROR, "open output file failed\n");
                return false;
            }
        }
        avformat_write_header(mContext, NULL);
        av_dump_format(mContext, 0, mContext->filename, 1);
        mMuxing = true;
        mThread = boost::thread(&Muxer::loop, this);
        av_log(NULL, AV_LOG_INFO, "muxer started - audio: %s, video: %s\n", mHasAudio?"true":"false", mHasVideo?"true":"false");
        return true;
    }
    av_log(NULL, AV_LOG_ERROR, "no stream to publish\n");
    return false;
}

bool Muxer::addVideoStream(enum AVCodecID codecId)
{
    AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "video codec not found\n");
        return false;
    }
    mContext->oformat->video_codec = codecId;
    mVideoStream = avformat_new_stream(mContext, codec);
    if (!mVideoStream) {
        av_log(NULL, AV_LOG_ERROR, "create new video stream failed\n");
        return false;
    }

    AVCodecContext* c = mVideoStream->codec;
    c->codec_id = codecId;
    c->codec_type = AVMEDIA_TYPE_VIDEO;
    /* Put sample parameters. */
    c->bit_rate = 400000;
    /* Resolution must be a multiple of two. */
    c->width    = 640;
    c->height   = 480;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    mVideoStream->time_base = (AVRational){ 1, 30 };
    c->time_base             = mVideoStream->time_base;

    c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
    c->pix_fmt       = AV_PIX_FMT_YUV420P;
    /* Some formats want stream headers to be separate. */
    if (mContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(c, NULL, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "open video codec failed\n");
        return false;
    }
    return true;
}

bool Muxer::addAudioStream(enum AVCodecID codecId)
{
    AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "audio codec not found\n");
        return false;
    }

    if (!(mAudioStream = avformat_new_stream(mContext, codec))) {
        av_log(NULL, AV_LOG_ERROR, "create new audio stream failed\n");
        return false;
    }

    AVCodecContext* c = mAudioStream->codec;

    /* put sample parameters */
    c->bit_rate = 96000;

    /* check that the encoder supports s16 pcm input */
    c->sample_fmt = AV_SAMPLE_FMT_S16;
    if (!check_sample_fmt(codec, c->sample_fmt)) {
        av_log(NULL, AV_LOG_ERROR, "encoder does not support %s\n", av_get_sample_fmt_name(c->sample_fmt));
        return false;
    }

    /* select other audio parameters supported by the encoder */
#ifndef AAC_ENC
    c->channels       = 1;
#else
    c->channels       = 2;
#endif
    c->channel_layout = av_get_default_channel_layout(c->channels);
#ifdef AAC_ENC
    c->sample_rate    = 44100;
#else
    c->sample_rate    = 8000;
#endif

    /** Set the sample rate for the container. */
    mAudioStream->time_base.den = 44100; //input_codec_context->sample_rate;
    mAudioStream->time_base.num = 1;

//    mAudioStream->time_base = (AVRational){ 1, c->sample_rate };
    /*c->time_base             = mAudioStream->time_base;*/
    if (mContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(c, NULL, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "open audio codec failed\n");
        return false;
    }
#ifndef AAC_ENC
    if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
        c->frame_size = 10000;
#endif
    return true;
}

int Muxer::writeVideoFrame(AVFrame* frame, int pts)
{
    AVPacket pkt = { 0 };
    av_init_packet(&pkt);
    fill_yuv_image(frame, pts, mVideoStream->codec->width, mVideoStream->codec->height);
    frame->pts = pts;
    int got_packet = 0;
    if (avcodec_encode_video2(mVideoStream->codec, &pkt, frame, &got_packet) < 0) {
        av_log(NULL, AV_LOG_ERROR, "error encoding a video frame\n");
        av_free_packet(&pkt);
        return 1;
    }
    if (got_packet) {
        av_packet_rescale_ts(&pkt, mVideoStream->codec->time_base, mVideoStream->time_base);
        pkt.stream_index = mVideoStream->index;
        return av_interleaved_write_frame(mContext, &pkt);
    }
    return 0;
}

int Muxer::writeAudioFrame(AVFrame* frame, int pts, uint8_t* buffer)
{
    AVPacket pkt = { 0 };

#if 0
    int got_output = 0;
    float t = 0, tincr = 2 * M_PI * 440.0 / mAudioStream->codec->sample_rate;

    for (int j = 0; j < mAudioStream->codec->frame_size; j++) {
        buffer[2*j] = (int)(sin(t) * 10000);

        for (int k = 1; k < mAudioStream->codec->channels; k++)
            buffer[2*j + k] = buffer[2*j];
        t += tincr;
    }
#endif
    frame->pts = pts;
#if 0
    /* encode the samples */
    if (avcodec_encode_audio2(mAudioStream->codec, &pkt, frame, &got_output) < 0) {
        av_log(NULL, AV_LOG_ERROR, "error encoding a audio frame\n");
        av_free_packet(&pkt);
        return 1;
    }
#endif

#ifdef FAKE_AAC_ENC
    int got_output = 1;

    if (index == AAC_INDEX - 1) {
      index = 0;
    }

  audioData = audios[index];

  {
    int size = audioData->getAudioSize();
    av_init_packet(&pkt);
    printf("packet size = %d\n", size);
    pkt.data     = (unsigned char*)audioData->getAudioData();//buf->data;
    pkt.size     = size;
  }
  index++;

  audios.pop_back();
#endif

#ifdef AAC_ENC
  // re-encode to aac
  /**
   * If we have enough samples for the encoder, we encode them.
   * At the end of the file, we pass the remaining samples to
   * the encoder.
   */
        while (av_audio_fifo_size(fifo) >= output_frame_size ||
               (finished && av_audio_fifo_size(fifo) > 0))
            /**
             * Take one frame worth of audio samples from the FIFO buffer,
             * encode it and write it to the output file.
             */
            if (load_encode_and_write(fifo, output_format_context,
                                      output_codec_context, mAudioStream, mContext))
                goto cleanup;

        /**
         * If we are at the end of the input file and have encoded
         * all remaining samples, we can exit this loop and finish.
         */
        if (finished) {
            int data_written;
            /** Flush the encoder as it may have delayed frames. */
            do {
                if (encode_audio_frame(NULL, output_format_context,
                                       output_codec_context, &data_written, mAudioStream, mContext))
                    goto cleanup;
            } while (data_written);
        }

  // copy buf to AVPacket for streaming

#endif
  return 0;
#if 0
    if (got_output) {
        av_packet_rescale_ts(&pkt, mAudioStream->codec->time_base, mAudioStream->time_base);
        pkt.stream_index = mAudioStream->index;
        return av_interleaved_write_frame(mContext, &pkt);
    }
#endif
  cleanup:
    if (fifo)
        av_audio_fifo_free(fifo);
    swr_free(&resample_context);
    if (output_codec_context)
        avcodec_close(output_codec_context);
    if (output_format_context) {
        avio_closep(&output_format_context->pb);
        avformat_free_context(output_format_context);
    }
    if (input_codec_context)
        avcodec_close(input_codec_context);
    if (input_format_context)
        avformat_close_input(&input_format_context);

    return 0;
}

void Muxer::loop()
{
    int apts = 0;
    int vpts = 0;
    AVFrame* vFrame = NULL;
    AVFrame* aFrame = NULL;
    uint8_t* samples = NULL;
    if (mHasVideo) {
        vFrame = alloc_video_frame(mVideoStream->codec);
        if (!vFrame) {
            av_log(NULL, AV_LOG_ERROR, "allocate video frame failed\n");
            mMuxing = false;
            return;
        }
    }
    if (mHasAudio) {
        aFrame = alloc_audio_frame(mAudioStream->codec, &samples);
        if (!aFrame) {
            av_log(NULL, AV_LOG_ERROR, "allocate audio frame failed\n");
            mMuxing = false;
            return;
        }
    }

    while (mMuxing) {
        usleep(30000);
        if (mHasAudio && mHasVideo) {
            if (av_compare_ts(vpts, mVideoStream->codec->time_base, apts, mAudioStream->codec->time_base) <= 0) {
                writeVideoFrame(vFrame, vpts);
                vpts++;
            } else {
                writeAudioFrame(aFrame, apts, samples);
                apts += aFrame->nb_samples;
            }
        } else if (mHasVideo) {
            writeVideoFrame(vFrame, vpts);
            vpts++;
        } else if (mHasAudio) {
          // decode pcmu
          /** Use the encoder's desired frame size for processing. */
        // see libavcodec/aacenc.c, avctx->frame_size = 1024;
        output_frame_size = output_codec_context->frame_size;
        finished                = 0;

        /**
         * Make sure that there is one frame worth of samples in the FIFO
         * buffer so that the encoder can do its work.
         * Since the decoder's and the encoder's frame size may differ, we
         * need to FIFO buffer to store as many frames worth of input samples
         * that they make up at least one frame worth of output samples.
         */
        while (av_audio_fifo_size(fifo) < output_frame_size) {
            /**
             * Decode one frame worth of audio samples, convert it to the
             * output sample format and put it into the FIFO buffer.
             */
            if (read_decode_convert_and_store(fifo, input_format_context,
                                              input_codec_context,
                                              output_codec_context,
                                              resample_context, &finished)) {
                printf("read_decode_convert_and_store failed!!\n");
                return;
            }

            /**
             * If we are at the end of the input file, we continue
             * encoding the remaining audio samples to the output file.
             */
            if (finished)
                break;
        }


            writeAudioFrame(aFrame, apts, samples);
//            printf("aFrame->nb_samples = %d, apts = %d\n", aFrame->nb_samples, apts);
            apts += 100;//aFrame->nb_samples;
        }
    }

    if (samples)
        av_freep(&samples);
    if (aFrame)
        av_frame_free(&aFrame);
    if (vFrame)
        av_frame_free(&vFrame);
}

int main(int argc, char **argv)
{
    av_register_all();
    avformat_network_init();

#ifdef FAKE_PCMU
  char file[32];
  for (int i = 0; i < PCMU_INDEX; i++) {
    sprintf(file, "dump_files/dump_pcmu_%d", i);
    ifstream in;
    in.open(file, ios_base::binary);
    in.seekg(0,ios::end);
    long size = in.tellg();
//    printf("%s, frame %d, size is %d\n", file, i, (int)size);
    ifstream fin;
    fin.open(file, ios_base::binary);
    //    char *tmp = new char[size];
    audioPCMUData = new AudioData((int)size);
    fin.read(audioPCMUData->getAudioData(), size);
    fin.close();
    pcmu_audios.push_back(audioPCMUData);
    in.close();
  }
#endif


    /** Open the input file for reading. */
    if (open_input_file(argv[1],
                        &input_format_context,
                        &input_codec_context))
        goto cleanup;
    /** Open the output file for writing. */
    if (open_output_file(argv[2],
                         input_codec_context,
                         &output_format_context, &output_codec_context))
        goto cleanup;
    /** Initialize the resampler to be able to convert audio sample formats. */
    if (init_resampler(input_codec_context, output_codec_context,
                       &resample_context))
        goto cleanup;
    /** Initialize the FIFO buffer to store audio samples to be encoded. */
    if (init_fifo(&fifo, output_codec_context))
        goto cleanup;
    /** Write the header of the output file container. */
    if (write_output_file_header(output_format_context))
        goto cleanup;

    {
    av_log_set_level(AV_LOG_DEBUG);

    Muxer* m = new Muxer("rtsp", "rtsp://10.239.10.45:8888/live.sdp");
    //Muxer* m = new Muxer("rtsp", "rtsp://10.239.10.117:1935/live/live.sdp");
    //Muxer* m = new Muxer(NULL, "abc.mkv");
    if (!m->start())
        return 1;
    int cycles = 5;
    while (cycles-- >= 0) {
        av_log(NULL, AV_LOG_DEBUG, ".");
        sleep(5);
    }
    av_log(NULL, AV_LOG_DEBUG, "\n");

    delete m;
    }

cleanup:
    if (fifo)
        av_audio_fifo_free(fifo);
    swr_free(&resample_context);
    if (output_codec_context)
        avcodec_close(output_codec_context);
    if (output_format_context) {
        avio_closep(&output_format_context->pb);
        avformat_free_context(output_format_context);
    }
    if (input_codec_context)
        avcodec_close(input_codec_context);
    if (input_format_context)
        avformat_close_input(&input_format_context);

    return 0;
}
