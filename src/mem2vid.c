#include <mem2vid/mem2vid.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

typedef struct {
    AVFormatContext *output_context;
    AVStream *stream;
    AVCodecContext *enc;
    AVFrame *frame;
    struct SwsContext *sws_context;
    AVCodec *codec;
/** index of the next frame */
    uint32_t frame_index;
} video_t;

/** for simplicity, allow only for one active video at a time */
static bool m_initialized = false;
static video_t m_video;

/**
 * receive packets and write to file
 * is called after sending frame or after all frames have been sent */
static int flush_packets(video_t *p_video);

int video_start(const char *name, video_param_t param)
{
    int ret;
    if (m_initialized) {
        fprintf(stderr, "video already started\n");
        goto cleanup_none;
    }
    video_t *video = &m_video;

    // set all data to default
    video_t empty = {0};
    *video = empty;

    /** append the filename with .mp4 */
    uint32_t len = strlen(name);
    const char ext[] = ".mp4";
    char *filename = calloc(sizeof(ext) + len, sizeof(char));
    strcat(filename, name);
    strcat(filename, ext); // ext is null-terminated

    /** add video stream using default format codec, and initialize the codec */
    // allocate output format (use format_name to find mp4)
    avformat_alloc_output_context2(&video->output_context, NULL, "mp4", NULL);
    if (!video->output_context) {
        fprintf(stderr, "could not allocate output format\n");
        goto cleanup_name;
    }

    // find video encoder
    video->codec = avcodec_find_encoder(video->output_context->oformat->video_codec);
    if (!video->codec) {
        fprintf(stderr, "could not find encoder\n");
        goto cleanup_name_context;
    }

    // create stream
    video->stream = avformat_new_stream(video->output_context, NULL);
    if (!video->stream) {
        fprintf(stderr, "could not create stream\n");
        goto cleanup_name_context;
    }
    video->stream->id = (int) (video->output_context->nb_streams - 1);

    // allocate video codec context
    video->enc = avcodec_alloc_context3(video->codec);
    if (!video->enc) {
        fprintf(stderr, "could not allocate video codec context\n");
        goto cleanup_name_context_stream;
    }

    video->enc->codec_id = video->output_context->oformat->video_codec;
    video->enc->bit_rate = param.bitrate * 1000000; // x Mbps
    video->enc->width = param.size_x;
    video->enc->height = param.size_y;
    video->stream->time_base = (AVRational) {.num=1, .den=param.frames_per_second};
    video->enc->time_base = video->stream->time_base;
    video->enc->pix_fmt = AV_PIX_FMT_YUV420P;
    // https://support.google.com/youtube/answer/1722171
    video->enc->gop_size = (signed) param.frames_per_second / 2;
    video->enc->max_b_frames = 2;

    // some formats want stream headers to be separate
    if (video->output_context->oformat->flags & AVFMT_GLOBALHEADER) {
        video->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    /** open video codec and allocate necessary encode buffers */
    ret = avcodec_open2(video->enc, video->codec, NULL);
    if (ret != 0) {
        fprintf(stderr, "could not open codec: %s\n", av_err2str(ret));
        goto cleanup_name_context_stream_context;
    }

    // allocate video frame
    video->frame = av_frame_alloc();
    if (!video->frame) {
        fprintf(stderr, "could not allocate video frame\n");
        goto cleanup_name_context_stream_context;
    }
    video->frame->format = video->enc->pix_fmt;
    video->frame->width = video->enc->width;
    video->frame->height = video->enc->height;

    // allocate the buffers for the frame data
    ret = av_frame_get_buffer(video->frame, 32);
    if (ret < 0) {
        fprintf(stderr, "could not allocate the video frame data\n");
        goto cleanup_name_context_stream_context_frame;
    }

    // copy the stream parameters to the muxer
    ret = avcodec_parameters_from_context(video->stream->codecpar, video->enc);
    if (ret < 0) {
        fprintf(stderr, "could not copy the stream parameters\n");
        goto cleanup_name_context_stream_context_frame;
    }

    // allocate conversion context
    video->sws_context = sws_getContext(
            video->enc->width, video->enc->height, AV_PIX_FMT_RGB24,   // src
            video->enc->width, video->enc->height, AV_PIX_FMT_YUV420P, // dst
            SWS_BICUBIC, NULL, NULL, NULL
    );
    if (!video->sws_context) {
        fprintf(stderr, "could not initialize the conversion context\n");
        goto cleanup_name_context_stream_context_frame;
    }

    // print some info
    av_dump_format(video->output_context, 0, filename, 1);

    /** open output file */
    // NB: skip checking AVFMT_NOFILE flag, we always want a file
    ret = avio_open(&video->output_context->pb, filename, AVIO_FLAG_WRITE);
    if (ret != 0) {
        fprintf(stderr, "could not open %s\n", filename);
        goto cleanup_name_context_stream_context_frame_sws;
    }

    /** write stream header */
    // write the stream header, if any
    ret = avformat_write_header(video->output_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "could not write \n");
        goto cleanup_all;
    }

    /** misc initialization */
    video->frame_index = 0;

    free(filename);
    m_initialized = true;

    return EXIT_SUCCESS;

    /** cleanup in case initialization fails */
    // NB: duplicate tail as {video_finish}
    cleanup_all:
    // close output file
    ret = avio_close(video->output_context->pb);
    if (ret != 0) {
        fprintf(stderr, "failed to close file\n");
    }
    cleanup_name_context_stream_context_frame_sws:
    // free swscaler context
    sws_freeContext(video->sws_context);
    cleanup_name_context_stream_context_frame:
    // free frame
    av_frame_free(&video->frame);
    cleanup_name_context_stream_context:
    // free context
    avcodec_free_context(&video->enc);
    cleanup_name_context_stream:
    // close stream
    avcodec_close(video->enc);
    cleanup_name_context:
    // cleanup context
    avformat_free_context(video->output_context);
    cleanup_name:
    // free filename
    free(filename);
    cleanup_none:

    return EXIT_SUCCESS;
}

void video_finish()
{
    int ret;
    if (!m_initialized) {
        fprintf(stderr, "video is not started\n");
        return;
    }
    video_t *video = &m_video;

    /** flush out encoder to receive delayed frames */
    // indicate that no more frames will be sent
    avcodec_send_frame(video->enc, NULL);
    flush_packets(video);

    /** write stream trailer */
    av_write_trailer(video->output_context);

    /** perform cleanup, in order of allocation */
    // NB: duplicate tail as {video_start}
    ret = avio_close(video->output_context->pb);
    if (ret != 0) {
        fprintf(stderr, "failed to close file\n");
    }
    sws_freeContext(video->sws_context);
    av_frame_free(&video->frame);
    avcodec_free_context(&video->enc);
    avcodec_close(video->enc);
    avformat_free_context(video->output_context);

    m_initialized = false;
}

int video_submit(const uint8_t *rgb)
{
    int ret;
    if (!m_initialized) {
        fprintf(stderr, "video is not started\n");
        return EXIT_FAILURE;
    }
    video_t *video = &m_video;

    /** prepare the frame */
    // make sure the frame data is writable
    ret = av_frame_make_writable(video->frame);
    if (ret < 0) {
        fprintf(stderr, "frame not writable\n");
        return EXIT_FAILURE;
    }

    // single line of all three components
    const int in_linesize[1] = {video->enc->width * 3};

    // convert the rgb data to ycbcr format
    sws_scale(
            video->sws_context,
            &rgb, in_linesize, 0, video->enc->height,  // src
            video->frame->data, video->frame->linesize // dst
    );
    video->frame->pts = video->frame_index++;

    /** encode the image */
    // send the frame to the encoder
    ret = avcodec_send_frame(video->enc, video->frame);
    if (ret < 0) {
        fprintf(stderr, "error sending a frame for encoding\n");
        return EXIT_FAILURE;
    }

    // write received frames
    return flush_packets(video);
}

static int flush_packets(video_t *p_video)
{
    int ret;
    do {
        AVPacket pkt = {0};

        ret = avcodec_receive_packet(p_video->enc, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "error encoding a frame: %s\n", av_err2str(ret));

            return EXIT_FAILURE;
        }

        // rescale output packet timestamp values from codec to stream timebase
        av_packet_rescale_ts(&pkt, p_video->enc->time_base, p_video->stream->time_base);
        pkt.stream_index = p_video->stream->index;

        // write the compressed frame to the media file
        ret = av_interleaved_write_frame(p_video->output_context, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) {
            fprintf(stderr, "error while writing output packet: %s\n", av_err2str(ret));

            return EXIT_FAILURE;
        }
    } while (ret >= 0);

    return EXIT_SUCCESS;
}