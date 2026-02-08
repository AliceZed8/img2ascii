#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>


const char PIXELS[] = " .,-=+*#%@";
const int PIXELS_NUM = sizeof(PIXELS) - 1;

float rgb2gray(unsigned char r, unsigned char g, unsigned char b) {
    return 0.299*r + 0.587*g + 0.114*b;
} 

char get_char_by_depth(float depth) {
    return PIXELS[(int) ((depth / 255.0f) * PIXELS_NUM)];
}

void get_terminal_size(int *rows, int *cols) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
        *rows = 0;
        *cols = 0;
        return;
    }
    *rows = w.ws_row;
    *cols = w.ws_col;
}

void clear_screen() {
    printf("\033[H\033[J");
}

void move_cursor_to_top() {
    printf("\033[H");
}


void process_frame(AVFrame *frame, int out_width, int out_height) {
    if (frame == NULL || frame->data[0] == NULL) return;
    unsigned char *img = frame->data[0];
    int width = frame->width;
    int height = frame->height;
    int linesize = frame->linesize[0];
    int channels = 3;

    float step_y = (float) height / (float) out_height;
    float step_x = (float) width / (float) out_width;

    int pix_index, char_index;
    int s_x, s_y;
    float gray;

    for (int y = 0; y < out_height; y++) {
        for (int x = 0; x < out_width; x++) {
            s_x = (int) (x * step_x);
            s_y = (int) (y * step_y);
            if (s_x >= width) s_x = width - 1;
            if (s_y >= height) s_y = height - 1;
            pix_index = s_y*linesize + s_x*channels;
            gray = rgb2gray(img[pix_index], img[pix_index + 1], img[pix_index + 2]);
            printf("%c", get_char_by_depth(gray));
        }
        printf("\n");
    }
}


int process_media(char *file_path, int term_rows, int term_cols) {
    // open
    AVFormatContext *format_context = NULL;
    if (avformat_open_input(&format_context, file_path, NULL, NULL) != 0) {
        printf("Failed to open file\n");
        return 1;
    }


    // find stream
    if (avformat_find_stream_info(format_context, NULL) < 0) {
        printf("Failed to find stream info\n");
        avformat_close_input(&format_context);
        return 1;
    }

    int video_stream = -1;
    for (int i = 0; i < format_context->nb_streams; i++) {
        if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = i;
            break;
        }
    }

    if (video_stream == -1) {
        printf("Failed to find video stream\n");
        avformat_close_input(&format_context);
        return 1;
    }

    // codec params
    AVCodecParameters *codec_params = format_context->streams[video_stream]->codecpar;
    
    // codec
    const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
    if (codec == NULL) {
        printf("Failed to find decoder\n");
        avformat_close_input(&format_context);
        return 1;
    }

    // codec context
    AVCodecContext *codec_context = avcodec_alloc_context3(codec);
    if (codec_context == NULL) {
        printf("Failed to alloc codec context\n");
        avformat_close_input(&format_context);
        return 1;
    }
    if (avcodec_parameters_to_context(codec_context, codec_params) < 0) {
        printf("Failed to copy codec params\n");
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return 1;
    }

    if (avcodec_open2(codec_context, codec, NULL) < 0) {
        printf("Failed to open codec\n");
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return 1;
    }


    // frame width
    int width = codec_context->width;
    int height = codec_context->height;
    float char_aspect = 0.5f;
    float img_aspect = (float) width / (float) height;
    int out_height = term_rows - 1;
    int out_width = (int) (out_height * img_aspect / char_aspect);


    // Sws
    struct SwsContext *sws_context = sws_getContext(
        width, height, codec_context->pix_fmt, 
        width, height, AV_PIX_FMT_RGB24, 
        SWS_BILINEAR | SWS_ACCURATE_RND,
        NULL, NULL, NULL);

    if (sws_context == NULL) {
        printf("Failed to create SwsContext\n");
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return 1;
    }


    // frames
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    if (frame == NULL || rgb_frame == NULL) {
        printf("Failed to allocate frame\n");
        sws_free_context(&sws_context);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return 1;
    }

    // rgb buffer
    int rgb_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codec_context->width, codec_context->height, 32);
    uint8_t *rgb_buffer = (uint8_t *)av_malloc(rgb_buffer_size * sizeof(uint8_t));
    if (rgb_buffer == NULL) {
        printf("Failed to allocate RGB buffer\n");
        sws_freeContext(sws_context);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return 1;
    }


    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer, AV_PIX_FMT_RGB24, codec_context->width, codec_context->height, 32);

    rgb_frame->width = width;
    rgb_frame->height = height;
    rgb_frame->format = AV_PIX_FMT_RGB24;



    // packet
    AVPacket *packet = av_packet_alloc();
    if (packet == NULL) {
        printf("Failed to allocate packet\n");
        av_free(rgb_buffer);
        sws_freeContext(sws_context);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return 1;
    }

    // fps
    double fps = av_q2d(format_context->streams[video_stream]->avg_frame_rate);
    if (fps <= 0) fps = 24.0;
    long frame_delay = (long)(1000000 / fps);

    // read frames
    while (av_read_frame(format_context, packet) >= 0) {
        if (packet->stream_index == video_stream) {
            int response = avcodec_send_packet(codec_context, packet);
            if (response < 0) {
                printf("Failed to send packet\n");
                break;
            }
            
            while (response >= 0) {
                response = avcodec_receive_frame(codec_context, frame);
                if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
                    break;
                } else if (response < 0) {
                    printf("Error receiving frame\n");
                    break;
                }
                // convert to rgb
                sws_scale(sws_context, (const uint8_t *const *) frame->data, frame->linesize, 0, codec_context->height, rgb_frame->data, rgb_frame->linesize);
                
                // print
                //clear_screen();
                move_cursor_to_top();
                process_frame(rgb_frame, out_width, out_height);
                fflush(stdout);
                
                usleep(frame_delay);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    // cleanup
    av_free(rgb_buffer);
    av_packet_free(&packet);
    av_frame_free(&rgb_frame);
    av_frame_free(&frame);
    sws_freeContext(sws_context);
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);;
    return 0;
}





int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: img2ascii <input>\n");
        return 1;
    }

    // term size
    int t_rows, t_cols;
    get_terminal_size(&t_rows, &t_cols);
    if (t_rows == 0 || t_cols == 0) {
        t_rows = 40;
        t_cols = 120;
    }

    
    process_media(argv[1], t_rows, t_cols);
    return 0;
}
