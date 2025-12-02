#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// 640x480 NV12
static const int W = 640;
static const int H = 480;

// memfd 替代 dma-buf
int create_fake_dma_buf(size_t size)
{
    int fd = memfd_create("fake_nv12", 0);
    if (fd < 0) return -1;
    ftruncate(fd, size);
    return fd;
}

// 简单生成 checkerboard NV12
void fill_nv12(uint8_t* ptr, int w, int h)
{
    uint8_t* y = ptr;
    uint8_t* uv = ptr + w*h;

    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            y[j*w + i] = ((i/32 + j/32) % 2) ? 200 : 50;

    for (int j = 0; j < h/2; j++)
        for (int i = 0; i < w/2; i++) {
            uv[j*w + 2*i]     = 90;   // U
            uv[j*w + 2*i + 1] = 240;  // V
        }
}

const char* vs_src =
"attribute vec2 aPos;"
"attribute vec2 aTex;"
"varying vec2 vTex;"
"void main(){"
"    gl_Position = vec4(aPos, 0.0, 1.0);"
"    vTex = aTex;"
"}";

const char* fs_src =
"#extension GL_OES_EGL_image_external : require\n"
"precision mediump float;"
"uniform samplerExternalOES texNV12;"
"varying vec2 vTex;"
"void main(){"
"    gl_FragColor = texture2D(texNV12, vTex);"
"}";

GLuint compile(GLenum type, const char* src){
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

int main()
{
    // -------------------------
    // 1. 创建 EGL + GLES2 环境
    // -------------------------
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, NULL, NULL);

    EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);

    EGLint pbuf_attr[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbuf_attr);

    EGLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    eglMakeCurrent(dpy, surf, surf, ctx);

    printf("EGL/GLES2 initialized.\n");

    // -------------------------
    // 2. 创建假 dma-buf 文件
    // -------------------------
    size_t nv12_size = W * H * 3 / 2;
    int dma_fd = create_fake_dma_buf(nv12_size);
    uint8_t* mapped = (uint8_t*)mmap(NULL, nv12_size, PROT_WRITE, MAP_SHARED, dma_fd, 0);

    fill_nv12(mapped, W, H);
    munmap(mapped, nv12_size);
    printf("Fake NV12 dma-buf created.\n");

    // -------------------------
    // 3. 用 EGL 创建 EGLImage
    // -------------------------
    EGLint img_attr[] = {
        EGL_WIDTH,          W,
        EGL_HEIGHT,         H,
        EGL_LINUX_DRM_FOURCC_EXT, 0x3231564E, // "NV12"
        EGL_DMA_BUF_PLANE0_FD_EXT,   dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  W,
        EGL_DMA_BUF_PLANE1_FD_EXT,     dma_fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, W*H,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,  W,
        EGL_NONE
    };

    EGLImageKHR img = eglCreateImage(
        dpy, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        NULL,
        img_attr
    );

    if (img == EGL_NO_IMAGE_KHR) {
        printf("eglCreateImage failed.\n");
        return -1;
    }
    printf("EGLImage created.\n");

    // -------------------------
    // 4. 绑定成 external texture
    // -------------------------
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");

    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
    printf("EGLImage bound to external texture.\n");

    // -------------------------
    // 5. 创建着色器绘制
    // -------------------------
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aTex");
    glLinkProgram(prog);
    glUseProgram(prog);

    // 全屏三角形
    float quad[] = {
        -1,-1, 0,0,
         1,-1, 1,0,
        -1, 1, 0,1,
         1, 1, 1,1
    };

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), quad);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), quad+2);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    printf("Rendered.\n");

    close(dma_fd);
    return 0;
}
