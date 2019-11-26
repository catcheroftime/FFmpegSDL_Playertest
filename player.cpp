#include "player.h"

#include <iostream>
#include <functional>

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

#define __STDC_CONSTANT_MACROS

#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)

#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)

using namespace std;

int thread_exit=0;
int sfp_refresh_thread(void *opaque){
    thread_exit=0;
    while (!thread_exit) {
        SDL_Event event;
        event.type = SFM_REFRESH_EVENT;
        SDL_PushEvent(&event);
        SDL_Delay(40);
    }
    thread_exit=0;
    //Break
    SDL_Event event;
    event.type = SFM_BREAK_EVENT;
    SDL_PushEvent(&event);

    return 0;
}


Player::Player()
  : m_videoindex(-1)
  , m_audioindex(-1)
{
}

void Player::play(char *url)
{
    m_pUrl = url;
    init();
}

void Player::init()
{
    av_register_all();
    avformat_network_init();
    m_pFormatCtx = avformat_alloc_context();

    if (avformat_open_input(&m_pFormatCtx, m_pUrl, NULL, NULL) !=0 ) {
        cout << "Could not open input file." << endl;
        return;
    }

    if (avformat_find_stream_info(m_pFormatCtx, NULL)<0 ) {
        cout << "Could not find stream infomation." << endl;
        return;
    }

    av_dump_format(m_pFormatCtx, 0, m_pUrl, 0);

    for (unsigned int i=0; i<m_pFormatCtx->nb_streams; i++) {
        if(m_pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO && m_videoindex<0){
            m_videoindex=i;
            continue;
        }
        if(m_pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO && m_audioindex<0){
            m_audioindex=i;
            continue;
        }
    }

    if (m_videoindex==-1) {
        cout << "Did not find a video stream." << endl;
        return;
    }

    if (m_audioindex==-1) {
        cout << "Did not find a audio stream." << endl;
        return;
    }

    m_pVCodecCtx = m_pFormatCtx->streams[m_videoindex]->codec;
    m_pVCodec = avcodec_find_decoder(m_pVCodecCtx->codec_id);
    if (m_pVCodec==NULL) {
        cout << "video codec not found." << endl;
        return;
    }
    if (avcodec_open2(m_pVCodecCtx,m_pVCodec, NULL)<0) {
        cout << "Could not open video codec";
        return;
    }

    m_pACodecCtx = m_pFormatCtx->streams[m_audioindex]->codec;
    m_pACodec = avcodec_find_decoder(m_pACodecCtx->codec_id);
    if (m_pACodec == NULL) {
        cout << "audio codec not found." << endl;
        return;
    }
    if (avcodec_open2(m_pACodecCtx,m_pACodec, NULL)<0) {
        cout << "Could not open audio codec";
        return;
    }
    packet_queue_init(&audioq);

    m_pFrame = av_frame_alloc();
    m_pFrameYUV = av_frame_alloc();
    m_out_buffer=(uint8_t *)av_malloc(avpicture_get_size(AV_PIX_FMT_YUV420P, m_pVCodecCtx->width, m_pVCodecCtx->height));
    avpicture_fill((AVPicture *)m_pFrameYUV, m_out_buffer, AV_PIX_FMT_YUV420P, m_pVCodecCtx->width, m_pVCodecCtx->height);
    m_packet=(AVPacket *)av_malloc(sizeof(AVPacket));

    // SDL video
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        cout << "Could not initialize SDL : " << SDL_GetError() << endl;
        return;
    }

    m_screen_w = m_pVCodecCtx->width;
    m_screen_h = m_pVCodecCtx->height;
    m_screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        m_screen_w, m_screen_h,SDL_WINDOW_OPENGL);

    if(!m_screen) {
        cout << "SDL: could not create window - exiting:" << SDL_GetError() << endl;
        return ;
    }
    m_sdlRenderer = SDL_CreateRenderer(m_screen, -1, 0);
    m_sdlTexture = SDL_CreateTexture(m_sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,m_pVCodecCtx->width,m_pVCodecCtx->height);

    m_sdlRect.x=0;
    m_sdlRect.y=0;
    m_sdlRect.w=m_pVCodecCtx->width;
    m_sdlRect.h=m_pVCodecCtx->height;

    m_dstrect.x=0;
    m_dstrect.y=0;
    m_dstrect.w=m_screen_w;
    m_dstrect.h=m_screen_w;

    // SDL audio
    SDL_AudioSpec wanted_spec;
    wanted_spec.freq = m_pACodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = m_pACodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = m_pACodecCtx;

    if (SDL_OpenAudio(&wanted_spec, NULL)<0) {
        cout << "SDL OpenAudio failure: " <<SDL_GetError() << endl;
        return;
    }
    SDL_PauseAudio(0);

    uint64_t out_chn_layout = AV_CH_LAYOUT_STEREO;
    enum AVSampleFormat out_sample_fmt=AV_SAMPLE_FMT_S16;
    int out_sample_rate=44100;
    int out_nb_samples = -1;
    int out_channels = -1;
    unsigned char *outBuff = NULL;
    uint64_t in_chn_layout = -1;

    out_nb_samples = m_pACodecCtx->frame_size;
    out_channels = av_get_channel_layout_nb_channels(out_chn_layout);
    out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples,  out_sample_fmt, 1);
    outBuff = (unsigned char *)av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE*2);
    printf("-------->out_buffer_size is %d\n",out_buffer_size);
    in_chn_layout = av_get_default_channel_layout(m_pACodecCtx->channels);

    audio_convert_ctx=swr_alloc_set_opts(NULL, out_chn_layout, out_sample_fmt, out_sample_rate, in_chn_layout, m_pACodecCtx->sample_fmt , m_pACodecCtx->sample_rate,  0,  NULL);

    uint8_t* video_out_buffer=(uint8_t *)av_malloc(avpicture_get_size(AV_PIX_FMT_YUV420P, m_pVCodecCtx->width, m_pVCodecCtx->height));
    avpicture_fill((AVPicture *)m_pFrameYUV, video_out_buffer, AV_PIX_FMT_YUV420P, m_pVCodecCtx->width, m_pVCodecCtx->height);
    img_convert_ctx = sws_getContext(m_pVCodecCtx->width, m_pVCodecCtx->height, m_pVCodecCtx->pix_fmt,
                                     m_pVCodecCtx->width, m_pVCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

    swr_init(audio_convert_ctx);


    SDL_CreateThread(sfp_refresh_thread,NULL,NULL);
    for (;;) {
        //Wait
        SDL_WaitEvent(&m_event);
        if(m_event.type==SFM_REFRESH_EVENT){
            while(av_read_frame(m_pFormatCtx, m_packet) >= 0)
            {
                if (m_packet->stream_index == m_videoindex)
                {
                    int got_picture = 0;
                    int ret = avcodec_decode_video2(m_pVCodecCtx, m_pFrame, &got_picture, m_packet);
                    if(ret < 0){
                        cout << "Decode Error." << endl;
                        return ;
                    }
                    if(got_picture){
                        sws_scale(img_convert_ctx, (const uint8_t* const*)m_pFrame->data, m_pFrame->linesize, 0, m_pVCodecCtx->height, m_pFrameYUV->data, m_pFrameYUV->linesize);

                        SDL_UpdateTexture( m_sdlTexture, NULL, m_pFrameYUV->data[0], m_pFrameYUV->linesize[0] );
                        SDL_RenderClear( m_sdlRenderer );
                        SDL_RenderCopy( m_sdlRenderer, m_sdlTexture, &m_sdlRect, &m_dstrect );

                        SDL_RenderPresent( m_sdlRenderer );
                    }
                } else if (m_packet->stream_index == m_audioindex)
                {
                    packet_queue_put(&audioq, m_packet);
                } else {
                    av_free_packet(m_packet);
                }
            }
        }
    }


    sws_freeContext(img_convert_ctx);
    swr_free(&audio_convert_ctx);

    SDL_CloseAudio();
    SDL_Quit();
    //--------------
    av_free(outBuff);
    av_free(video_out_buffer);

    av_frame_free(&m_pFrameYUV);
    av_frame_free(&m_pFrame);
    avcodec_close(m_pACodecCtx);
    avcodec_close(m_pVCodecCtx);
    avformat_close_input(&m_pFormatCtx);
}

void Player::audio_callback(void *userdata, Uint8 *stream, int len)
{
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
     int len1, audio_size;

     static uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
     static unsigned int audio_buf_size = 0;
     static unsigned int audio_buf_index = 0;

     while(len > 0)
     {
         if(audio_buf_index >= audio_buf_size)  //表示当前audio_buf中已经没有数据，需要解码数据了
         {
             /* We have already sent all our data; get more */
             audio_size = audio_decode_frame(aCodecCtx, audio_buf,sizeof(audio_buf));
             if(audio_size < 0)
             {
                 /* If error, output silence */
                 audio_buf_size = 1024;
                 memset(audio_buf, 0, audio_buf_size);
             }
             else
             {
                 audio_buf_size = audio_size;
             }
             audio_buf_index = 0;
         }

         len1 = audio_buf_size - audio_buf_index;
         if(len1 > len)
             len1 = len;

         memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
         len -= len1;
         stream += len1;
         audio_buf_index += len1;
     }
}

int Player::audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size)
{
    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    static AVFrame frame;
    int len1, data_size = 0;

    for(;;)
    {
        while(audio_pkt_size > 0)
        {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);
            if(len1 < 0)
            {
                audio_pkt_size = 0;
                break;
            }

            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;

            if(got_frame)
            {
                swr_convert(audio_convert_ctx,&audio_buf, AVCODEC_MAX_AUDIO_FRAME_SIZE,(const uint8_t **)frame.data , frame.nb_samples);
                data_size = out_buffer_size;
            }

            if(data_size <= 0)
                continue;

            return data_size;
        }

        if(pkt.data)
            av_free_packet(&pkt);

        if(quit)
            return -1;

        if(packet_queue_get(&audioq, &pkt, 1) < 0)
            return -1;


        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}


void Player::packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int Player::packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
    if (av_dup_packet(pkt)<0) {
        return -1;
    }

    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

int Player::packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;) {
        if (quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;

            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret =1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }

    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}



