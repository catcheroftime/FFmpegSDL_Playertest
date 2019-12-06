#include <stdio.h>
#include <iostream>

#include "definestruct.h"

using namespace std;
//#include "player.h"

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_refresh_timer(void *userdata)
{
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    if(is->video_st) {
        if(is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            vp = &is->pictq[is->pictq_rindex];
            /* Timing code goes here */

            schedule_refresh(is, 35);

            /* show the picture! */
            video_display(is);

            /* update queue for next picture! */
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

void decode_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;

    for(;;) {
        if(is->quit) {
            break;
        }
        // seek stuff goes here
        if(is->audioq.size > MAX_AUDIOQ_SIZE ||
                is->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        if(av_read_frame(is->pFormatCtx, packet) < 0) {
            if((is->pFormatCtx->pb->error) == 0) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // Is this a packet from the video stream?
        if(packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if(packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }
}

int video_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;
    int frameFinished;
    AVFrame *pFrame;

    pFrame = av_frame_alloc();

    for(;;) {
        if(packet_queue_get(&is->videoq, packet, 1) < 0) {
            break;
        }
        // Decode video frame
        avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

        // Did we get a video frame?
        if(frameFinished) {
            if(queue_picture(is, pFrame) < 0) {
                break;
            }
        }
        av_free_packet(packet);
    }
    av_free(pFrame);
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {

    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec ;

    if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }

    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
    if(!codec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if(avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
    }


    if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        // Set audio settings from codec info
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = codecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = 1024;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;

        if(SDL_OpenAudio(&wanted_spec, NULL) < 0) {
            cout << "SDL OpenAudio failure: " <<SDL_GetError() << endl;
            return -1;
        }
    }

    if(avcodec_open2(codecCtx, codec, NULL) < 0) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch(codecCtx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
    {
        is->audioStream = stream_index;
        is->audio_st = pFormatCtx->streams[stream_index];
        is->audio_ctx = codecCtx;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;


        uint64_t out_chn_layout = AV_CH_LAYOUT_STEREO;
        enum AVSampleFormat out_sample_fmt=AV_SAMPLE_FMT_S16;
        int out_sample_rate=44100;
        int out_nb_samples = codecCtx->frame_size;
        int out_channels = av_get_channel_layout_nb_channels(out_chn_layout);
        uint64_t in_chn_layout = av_get_default_channel_layout(codecCtx->channels);

        is->audio_buf_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples,  out_sample_fmt, 1);
        is->swr_ctx = swr_alloc_set_opts(NULL, out_chn_layout, out_sample_fmt, out_sample_rate,
                                       in_chn_layout, codecCtx->sample_fmt , codecCtx->sample_rate,  0,  NULL);

        swr_init(is->swr_ctx);

        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        packet_queue_init(&is->audioq);
        SDL_PauseAudio(0);
        break;
    }

    case AVMEDIA_TYPE_VIDEO:
        is->videoStream = stream_index;
        is->video_st = pFormatCtx->streams[stream_index];
        is->video_ctx = codecCtx;
        is->yuvframe = av_frame_alloc();
        is->video_out_buffer = (uint8_t *)av_malloc(avpicture_get_size(AV_PIX_FMT_YUV420P, is->video_ctx->width, is->video_ctx->height));
        avpicture_fill((AVPicture *)is->yuvframe, is->video_out_buffer, AV_PIX_FMT_YUV420P, is->video_ctx->width, is->video_ctx->height);
        packet_queue_init(&is->videoq);
        is->video_tid = SDL_CreateThread(video_thread,NULL, is);
        is->sws_ctx = sws_getContext(is->video_st->codec->width, is->video_st->codec->height,
                                     is->video_st->codec->pix_fmt, is->video_st->codec->width,
                                     is->video_st->codec->height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, NULL, NULL, NULL
                                     );

        is->screen_w = 500;
        is->screen_h = 500;
        is->screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            is->screen_w, is->screen_h,SDL_WINDOW_OPENGL);

        if(!is->screen) {
            cout << "SDL: could not create window - exiting:" << SDL_GetError() << endl;
            return ;
        }
        is->sdlRenderer = SDL_CreateRenderer(is->screen, -1, 0);
        is->sdlTexture = SDL_CreateTexture(is->sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,is->video_ctx->width,is->video_ctx->height);

        is->sdlRect.x=0;
        is->sdlRect.y=0;
        is->sdlRect.w=is->video_ctx->width;
        is->sdlRect.h=is->video_ctx->height;
        break;
    default:
        break;
    }
}


int main(int argc, char *args[])
{
    SDL_Event       event;
    VideoState      *is;
    is = av_mallocz(sizeof(VideoState));

    av_strlcpy(is->filename, args[1], sizeof(is->filename));

    is->pictq_mutex = SDL_CreateMutex();
    is->screen_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();

    av_register_all();
    avformat_network_init();
    is->pFormatCtx = avformat_alloc_context();

    if (avformat_open_input(&(is->pFormatCtx), is->filename, NULL, NULL) !=0 ) {
        cout << "Could not open input file." << endl;
        return;
    }

    if (avformat_find_stream_info(is->pFormatCtx, NULL)<0 ) {
        cout << "Could not find stream infomation." << endl;
        return;
    }

    av_dump_format(is->pFormatCtx, 0, is->filename, 0);

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        cout << "Could not initialize SDL : " << SDL_GetError() << endl;
        return;
    }

    for (unsigned int i=0; i<is->pFormatCtx->nb_streams; i++)
        stream_component_open(is, i);

    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(decode_thread, NULL, is);
    if(!is->parse_tid) {
        av_free(is);
        return -1;
    }


    for(;;) {
        SDL_WaitEvent(&event);
        switch(event.type)
        {
            case FF_REFRESH_EVENT:
            {
                video_refresh_timer(event.user.data1);
                break;
            }

            case SDL_QUIT:
            {
                SDL_CloseAudio();//Close SDL
                SDL_Quit();

                av_free(is);

                return;
            }
        }
    }


    return 0;
}




