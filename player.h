#ifndef PLAYER_H
#define PLAYER_H
#define SDL_MAIN_HANDLED

extern "C"
{
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

static int out_buffer_size = -1;
static int quit = 0;
static PacketQueue audioq;
static struct SwrContext *audio_convert_ctx;
static struct SwsContext *img_convert_ctx;

class Player
{
public:
    Player();

    void play(char *url);

private:
    void init();

    static void audio_callback(void *userdata, Uint8 *stream, int len);
    static int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size);

    static void packet_queue_init(PacketQueue *q);
    static int packet_queue_put(PacketQueue *q, AVPacket *pkt);
    static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

private:
    char *m_pUrl;



    AVFormatContext	*m_pFormatCtx;
    int m_videoindex, m_audioindex;
    AVCodecContext *m_pVCodecCtx;
    AVCodec *m_pVCodec;

    AVCodecContext *m_pACodecCtx;
    AVCodec *m_pACodec;

    AVFrame *m_pFrame, *m_pFrameYUV;
    uint8_t *m_out_buffer;
    AVPacket *m_packet;


    struct SwsContext *m_img_convert_ctx;

    int m_screen_w, m_screen_h;
    SDL_Window *m_screen;
    SDL_Renderer* m_sdlRenderer;
    SDL_Texture* m_sdlTexture;
    SDL_Rect m_sdlRect, m_dstrect;
    SDL_Thread *m_video_tid;
    SDL_Event m_event;

};

#endif // PLAYER_H
