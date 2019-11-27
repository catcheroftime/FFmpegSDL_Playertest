#ifndef DEFINESTRUCT_H
#define DEFINESTRUCT_H

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000
#define VIDEO_PICTURE_QUEUE_SIZE 10
#define SDL_MAIN_HANDLED

#define SDL_MAIN_HANDLED

#define MAX_AUDIOQ_SIZE 10
#define MAX_VIDEOQ_SIZE 10

#define FF_REFRESH_EVENT  (SDL_USEREVENT + 1)

extern "C"
{
#include "libavutil/avstring.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include <SDL2/SDL.h>
}

typedef struct PacketQueue {
    AVPacketList * first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


typedef struct VideoPicture {
    AVFrame *bmp;
    int width, height; /* source height & width */
    int allocated;
} VideoPicture;

typedef struct VideoState {

    AVFormatContext   *pFormatCtx;
    int               videoStream, audioStream;
    AVStream          *audio_st;
    AVCodecContext    *audio_ctx;
    PacketQueue       audioq;
    uint8_t           audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int      audio_buf_size;
    unsigned int      audio_buf_index;
    AVPacket          audio_pkt;
    uint8_t           *audio_pkt_data;
    int               audio_pkt_size;
    AVStream          *video_st;
    AVCodecContext    *video_ctx;
    PacketQueue       videoq;
    uint8_t           *video_out_buffer;

    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;

    AVFrame           *yuvframe;
    VideoPicture      pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int               pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex         *pictq_mutex;
    SDL_cond          *pictq_cond;

    SDL_Thread        *parse_tid;
    SDL_Thread        *video_tid;

    int               screen_w, screen_h;
    SDL_Window        *screen;
    SDL_mutex         *screen_mutex;
    SDL_Renderer      *sdlRenderer;
    SDL_Texture       *sdlTexture;
    SDL_Rect          sdlRect, dstrect;

    char              filename[1024];
    int               quit;
} VideoState;

void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
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

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;) {
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

void alloc_picture(void *userdata) {

  VideoState *is = (VideoState *)userdata;
  VideoPicture *vp;

  vp = &is->pictq[is->pictq_windex];
  if(vp->bmp)
    av_free(vp->bmp);

  vp->bmp = av_frame_alloc();

  vp->width = is->video_st->codec->width;
  vp->height = is->video_st->codec->height;
  vp->allocated = 1;
}

int queue_picture(VideoState *is, AVFrame *pFrame) {

    VideoPicture *vp;
    int dst_pix_fmt;
    AVPicture pict;

    /* wait until we have space for a new pic */
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
          !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if(is->quit)
        return -1;

    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];

    /* allocate or resize the buffer! */
    if(!vp->bmp || vp->width != is->video_st->codec->width || vp->height != is->video_st->codec->height) {
        SDL_Event event;
        vp->allocated = 0;
        alloc_picture(is);
        if(is->quit) {
            return -1;
        }
    }

    if(vp->bmp) {
        sws_scale(is->sws_ctx, (const uint8_t* const*)pFrame->data, pFrame->linesize, 0, is->video_ctx->height, is->yuvframe->data, is->yuvframe->linesize);

        memcpy(vp->bmp, is->yuvframe, sizeof(AVFrame));

        /* now we inform our display thread that we have a pic ready */
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

void video_display(VideoState *is)
{
    VideoPicture *vp;
    vp = &is->pictq[is->pictq_rindex];
    if(vp->bmp) {
//        SDL_LockMutex(is->screen_mutex);
        SDL_UpdateTexture( is->sdlTexture, NULL, vp->bmp->data[0], vp->bmp->linesize[0] );
        SDL_RenderClear( is->sdlRenderer );
        SDL_RenderCopy( is->sdlRenderer, is->sdlTexture, &is->sdlRect, &is->dstrect );
        SDL_RenderPresent( is->sdlRenderer );
//        SDL_UnlockMutex(is->screen_mutex);
    }
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size)
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
            len1 = avcodec_decode_audio4(is->audio_ctx, &frame, &got_frame, &pkt);
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
                swr_convert(is->swr_ctx,&audio_buf, AVCODEC_MAX_AUDIO_FRAME_SIZE,(const uint8_t **)frame.data , frame.nb_samples);
                data_size = is->audio_buf_size;
            }

            if(data_size <= 0)
                continue;

            return data_size;
        }

        if(pkt.data)
            av_free_packet(&pkt);

        if(is->quit)
            return -1;

        if(packet_queue_get(&(is->audioq), &pkt, 1) < 0)
            return -1;


        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}


void audio_callback(void *userdata, Uint8 *stream, int len)
{
     VideoState *is = (VideoState *)userdata;
     int len1, audio_size;

     static uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
     static unsigned int audio_buf_size = 0;
     static unsigned int audio_buf_index = 0;

     while(len > 0)
     {
         if(audio_buf_index >= audio_buf_size)  //表示当前audio_buf中已经没有数据，需要解码数据了
         {
             /* We have already sent all our data; get more */
             audio_size = audio_decode_frame(is, audio_buf,sizeof(audio_buf));
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

#endif // DEFINESTRUCT_H
