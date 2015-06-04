#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <math.h>
#include <err.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/un.h>
#include <sys/socket.h>

struct ASC_PRIV_DATA;
struct ASC {
    ASC_PRIV_DATA* priv_data;
    char* data;
    int size;
    int width;
    int height;
    char pixfmtName[32];
};

#define LOG(fmt, arg...)           _LOG(fmt, ##arg)
#define ABORT(fmt, arg...)       ({_LOG(fmt ". Now exit", ##arg); exit(0);})
#define ABORT_ERRNO(fmt, arg...) ({_LOG(fmt " [errno %d(%s)] Now exit", errno, strerror(errno), ##arg); exit(0);})

static void _LOG(const char* format, ...) {
    char buf[4096];
    int cnt;
    va_list va;
    struct timespec ct;
    struct tm * st;
    clock_gettime(CLOCK_REALTIME, &ct);
    st = localtime(&ct.tv_sec);
    cnt = sprintf(buf, "%02d/%02d %02d:%02d:%02d.%03d [ASC %d] ", st->tm_mon+1, st->tm_mday, st->tm_hour, st->tm_min, st->tm_sec, (int)(ct.tv_nsec/1000000), gettid());
    va_start(va, format);
    cnt += vsnprintf(buf+cnt, sizeof(buf)-cnt, format, va);
    va_end(va);
    if (cnt > sizeof(buf)) cnt = sizeof(buf); else if (cnt <= 0) {cnt = 7; strcpy(buf, "LogErr");};
    if (buf[cnt-1]==0) cnt--; //always true
    if (buf[cnt-1]!='\n') buf[cnt++] = '\n';
    write(STDERR_FILENO, buf, cnt);
}

// hack android OS head file
#include "libcutils.h"
#if (ANDROID_VER>=400)
    #include "libgui.h"
#endif
#include "libinline.h"
using namespace android;

static bool isPaused = false;
static Mutex mutex;
static Condition cond;

static bool isFirstTime = true;
static bool needLog = true;
#if (ANDROID_VER>=400)
    static ScreenshotClient screenshot;
    #if (ANDROID_VER>=420)
        static sp<IBinder> display;
    #endif
#else
    static int fb;
    static char* mapbase;
    static size_t lastMapSize;
#endif
static char* blackscreen = NULL;

static void chkDev() {
    char k[128] = {0};
    char sn[256] = {0};
    char hb[4+1] = {'0','0','0','0', 0};
    char now[6+1] = {0};
    const char* es;
    char* ds;
    char* err;
    int i=0, esLen, snLen, dsLen;
    unsigned int ec, sc, dc;
    struct timespec ct;
    struct tm * st;

    es=getenv("ASC_");
    if (!es || !es[0]) ABORT("!nes");
    esLen = strlen(es);
    if (esLen%(2*6) != 0) ABORT("!esl");
    dsLen = esLen/2;

    k[i++] = 'r'; k[i++] = 'o'; k[i++] = '.'; k[i++] = 's'; k[i++] = 'e'; k[i++] = 'r'; k[i++] = 'i'; k[i++] = 'a'; k[i++] = 'l'; k[i++] = 'n'; k[i++] = 'o';
    property_get(k, sn, " ");
    snLen = strlen(sn);

    if ( dsLen != (snLen+6-1)/6*6 ) ABORT("!esms %d %d %d %d", sn, snLen, (snLen+6-1)/6*6, dsLen);

    ds = (char*)calloc(dsLen+1, 1);
    for(i=0; i < dsLen; i++) {
        //hb[0] = hb[1] = '0';
        hb[2] = es[i*2];
        hb[3] = es[i*2+1];
        ec = (unsigned int)strtoul(hb, &err, 16);
        if (err&&err[0]) ABORT("!ec", err);
        sc = (unsigned int)(unsigned char)sn[i%snLen];
        dc = ec ^ sc;
        if (dc < '0' || dc > '9') ABORT("!dcd");
        ds[i] = (char)dc;
    }
    for (i = 6; i < dsLen; i += 6)
        if ( 0 != memcmp(ds, &ds[i], 6)) ABORT("!dsfd");

    clock_gettime(CLOCK_REALTIME, &ct);
    st = localtime(&ct.tv_sec);
    sprintf(now, "%02d%02d%02d", (st->tm_year+1900-2000), st->tm_mon+1, st->tm_mday);

    if (memcmp(now, ds, 6) > 0) ABORT("!to");
    free(ds);
}

static void handle_cmd_locked(unsigned char cmd) {
    AutoMutex autoLock(mutex);
    switch (cmd) {
    case '+': //start
        if (isPaused) {
            isPaused = false;
            cond.signal();
        }
        break;
    case '-': //pause
        if (!isPaused) {
            isPaused = true;
            cond.signal();
        }
        break;
    }
}

static void* thread_cmd_server_connection_handler(void* thd_param) {
    int connection_fd = (int)thd_param;
    for(;;) {
        unsigned char cmd;
        LOG("~~~~ reading cmd");
        if (read(connection_fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
            LOG("~~~~ can not read cmd");
            break;
        }
        LOG("~~~~ got cmd: %c (%d)", cmd, cmd);
        handle_cmd_locked(cmd);
    }
    close(connection_fd);
    return 0;
}

static int socket_server_fd;

static void* thread_socket_server(void* thd_param) {
    for(;;) {
        LOG("~~~~ accept");
        int connection_fd = accept(socket_server_fd, NULL, NULL);
        if (connection_fd == -1) ABORT_ERRNO("accept");
        LOG("~~~~ new connection come in");

        pthread_t thd;
        int err = pthread_create(&thd, NULL, thread_cmd_server_connection_handler, (void*)connection_fd);
        if (err) ABORT("pthread_create err %d", err);
    }
    return 0;
}

static void create_cmd_socket_server() {
    char* socket_name = getenv("ASC_SOCKET_NAME");
    if (socket_name && socket_name[0]) {
        LOG("~~~~ create socket server %s", socket_name);
        socket_server_fd = socket(AF_LOCAL, SOCK_STREAM, 0);
        if (socket_server_fd == -1) ABORT_ERRNO("socket");

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_LOCAL;
        int namelen = strlen(socket_name);
        if (1/*\0*/+namelen > sizeof(addr.sun_path)) ABORT("socket name too long");
        memcpy(&addr.sun_path[1], socket_name, namelen);
        int addrlen = offsetof(struct sockaddr_un, sun_path) + 1/*\0*/ + namelen;
        LOG("~~~~ namelen %d (%d %s) sun_path[0]:%d", namelen, strlen(&addr.sun_path[1]), &addr.sun_path[1], addr.sun_path[0]);

        if (bind(socket_server_fd, (struct sockaddr*)&addr, addrlen)) ABORT_ERRNO("bind");

        if (listen(socket_server_fd, 4)) ABORT_ERRNO("listen");

        pthread_t thd;
        int err = pthread_create(&thd, NULL, thread_socket_server, socket_name);
        if (err) ABORT("pthread_create err %d", err);
    }
}

extern "C" void asc_capture(ASC* asc) {
    int err, width, height, internal_width, bytesPerPixel, rawImageSize;

    if (isFirstTime) {
        chkDev();
        #if (ANDROID_VER>=400)
            #if (ANDROID_VER>=420)
                ProcessState::self()->startThreadPool();
                display = SurfaceComposerClient::getBuiltInDisplay(0 /*1 is hdmi*/);
                if (display==NULL) ABORT("gbd e");
            #endif
        #else
            LOG("o f");
            fb = open("/dev/graphics/fb0", O_RDONLY);
            if (fb < 0) ABORT_ERRNO("o f");
            mapbase = NULL;
            lastMapSize = 0;
        #endif


        if (isatty(STDOUT_FILENO)) {
            LOG("iaty");
            struct termios term;
            if (tcgetattr(STDOUT_FILENO, &term)) ABORT_ERRNO("tga");
            LOG("mkr");
            cfmakeraw(&term);
            LOG("tsa");
            if (tcsetattr(STDOUT_FILENO, TCSANOW, &term)) ABORT_ERRNO("tsa");
        }

        create_cmd_socket_server();
    }

    if (isPaused && !isFirstTime) {
        if (!blackscreen) {
            blackscreen = (char*)calloc(asc->size, 1);
            asc->data = blackscreen;
            return;
        }
        else {
            AutoMutex autoLock(mutex);
            if (isPaused) {
                cond.wait(mutex);
                if (blackscreen) {
                    free(blackscreen);
                    blackscreen = NULL;
                }
            }
        }
    }

    #if (ANDROID_VER>=400)
        if (needLog) LOG("c w %d h %d)", asc->width, asc->height);
        for(;;) {
            #if (ANDROID_VER>=500)
                err = screenshot.update(display, Rect(), asc->width, asc->height, false);
            #elif (ANDROID_VER>=420)
                err = screenshot.update(display, asc->width, asc->height);
            #else
                err = screenshot.update(asc->width, asc->height);
            #endif
            if(err) {
                if (needLog) LOG("c e %d", err);
                usleep(250*1000);
                if (!isFirstTime) {
                    if (!blackscreen) {
                        blackscreen = (char*)calloc(asc->size, 1);
                        asc->data = blackscreen;
                    }
                    return;
                }
            } else {
                if (blackscreen) {
                    free(blackscreen);
                    blackscreen = NULL;
                }
                break;
            }
        }

        rawImageSize = screenshot.getSize();
        width = screenshot.getWidth();
        height = screenshot.getHeight();
        internal_width = screenshot.getStride();
        bytesPerPixel = rawImageSize/internal_width/height;

        if (isFirstTime) {
            strncpy(asc->pixfmtName, 
                (bytesPerPixel==4) ? "rgb0" :
                (bytesPerPixel==3) ? "rgb24" :
                (bytesPerPixel==2) ? "rgb565le" :
                (bytesPerPixel==5) ? "rgb48le" :
                (bytesPerPixel==6) ? "rgba64le" :
                (LOG("s bbp %d", bytesPerPixel),"unknown"),
                sizeof(asc->pixfmtName)-1);

            LOG("c r %s is %d w %d h %d bbp %d iw %d f %d",
                asc->pixfmtName, width*height*bytesPerPixel, width, height, bytesPerPixel, internal_width, screenshot.getFormat());
        }

        asc->data = (char*)screenshot.getPixels();

        if (internal_width > width) {
            char* p1 = asc->data;
            char* p2 = asc->data;
            int size1 = width*bytesPerPixel;
            int size2 = internal_width*bytesPerPixel;
            for (int h=0; h < height; h++, p2 += size2, p1+= size1)
                memmove(p1, p2, size1);
        }
    #else
        if (needLog) LOG("ic gv");
        struct fb_var_screeninfo vinfo;
        if (ioctl(fb, FBIOGET_VSCREENINFO, &vinfo) < 0) ABORT_ERRNO("ic gv");

        width = vinfo.xres;
        height = vinfo.yres;
        internal_width = width;
        bytesPerPixel = vinfo.bits_per_pixel/8;
        rawImageSize = (width*height) * bytesPerPixel;

        if (isFirstTime) {
            if (vinfo.transp.offset==0&&vinfo.bits_per_pixel==32&&vinfo.red.offset==8) strncpy(asc->pixfmtName,"0rgb", sizeof(asc->pixfmtName)-1);
            else if (vinfo.transp.offset==0&&vinfo.bits_per_pixel==32&&vinfo.red.offset==24) strncpy(asc->pixfmtName,"0bgr", sizeof(asc->pixfmtName)-1);
            else
                strncpy(asc->pixfmtName,
                    (vinfo.bits_per_pixel==32&&vinfo.red.offset==0) ? "rgb0" :
                    (vinfo.bits_per_pixel==32&&vinfo.red.offset!=0) ? "bgr0" :
                    (vinfo.bits_per_pixel==24&&vinfo.red.offset==0) ? "rgb24" :
                    (vinfo.bits_per_pixel==24&&vinfo.red.offset!=0) ? "bgr24" :
                    (vinfo.bits_per_pixel==16&&vinfo.red.offset==0) ? "rgb565le" :
                    (vinfo.bits_per_pixel==16&&vinfo.red.offset!=0) ? "bgr565le" :
                    (vinfo.bits_per_pixel==48&&vinfo.red.offset==0) ? "rgb48le" :
                    (vinfo.bits_per_pixel==48&&vinfo.red.offset!=0) ? "bgr48le" :
                    (vinfo.bits_per_pixel==64&&vinfo.red.offset==0) ? "rgba64le" :
                    (vinfo.bits_per_pixel==64&&vinfo.red.offset!=0) ? "bgra64le" :
                    (LOG("strange bits_per_pixel:%d", vinfo.bits_per_pixel),"unknown"),
                    sizeof(asc->pixfmtName)-1);

            LOG("gv r %s is %d w %d h %d bpp %d vw %d vh %d"
                " bits:%d"
                " R:(offset:%d length:%d msb_right:%d)"
                " G:(offset:%d length:%d msb_right:%d)"
                " B:(offset:%d length:%d msb_right:%d)"
                " A:(offset:%d length:%d msb_right:%d)"
                " grayscale:%d nonstd:%d rotate:%d",
                asc->pixfmtName, rawImageSize, width, height, bytesPerPixel, vinfo.xres_virtual, vinfo.yres_virtual
                ,vinfo.bits_per_pixel
                ,vinfo.red.offset, vinfo.red.length, vinfo.red.msb_right
                ,vinfo.green.offset, vinfo.green.length, vinfo.green.msb_right
                ,vinfo.blue.offset, vinfo.blue.length, vinfo.blue.msb_right
                ,vinfo.transp.offset, vinfo.transp.length, vinfo.transp.msb_right
                ,vinfo.grayscale, vinfo.nonstd, vinfo.rotate );
        }

        uint32_t offset =  (vinfo.xoffset + vinfo.yoffset*width) *bytesPerPixel;
        int virtualSize = vinfo.xres_virtual*vinfo.yres_virtual*bytesPerPixel;
        if (offset+rawImageSize > virtualSize) {
            LOG("Strange! offset:%d+rawImageSize:%d > virtualSize:%d", offset, rawImageSize, virtualSize);
            virtualSize = offset+rawImageSize;
        }

        if (virtualSize > lastMapSize) {
            if (mapbase) {
                LOG("remap due to virtualSize %d is bigger than previous %d", virtualSize, lastMapSize);
                munmap(mapbase, lastMapSize);
                mapbase = NULL;
            }
            lastMapSize = virtualSize;
        }

        if (mapbase==NULL) {
            mapbase = (char*)mmap(0, virtualSize, PROT_READ, MAP_PRIVATE, fb, 0);
            if (mapbase==NULL) ABORT_ERRNO("mmap %d", virtualSize);
        }

        asc->data = mapbase + offset;
    #endif

    if (isPaused) {
        if (!blackscreen) {
            blackscreen = (char*)calloc(asc->size, 1);
            asc->data = blackscreen;
        }
    }

    if (isFirstTime) {
        asc->width = width;
        asc->height = height;
        asc->size = width*height*bytesPerPixel;

        if (! (getenv("ASC_LOG_ALL") && atoi(getenv("ASC_LOG_ALL")) > 0) )
            needLog = false;
        isFirstTime = false;
    }
}

#if MAKE_TEST==1
    extern "C" int main(int argc, char** argv) {
        ASC asc;
        memset(&asc, 0, sizeof(ASC));
        asc.width = argc>1 && atoi(argv[1])> 0 ? atoi(argv[1]) : 0;
        asc.height = argc>2 && atoi(argv[2])> 0 ? atoi(argv[2]) : 0;

        for(;;) {
            asc_capture(&asc);
            static int64_t seq = 0;
            LOG("o i %lld", ++seq);
            write(1, asc.data, asc.size);
        }
    }
#endif
