// nv12_gbm_egl.cpp
// Build (Ubuntu/Debian):
// sudo apt install build-essential libegl1-mesa-dev libgles2-mesa-dev libx11-dev libgbm-dev libdrm-dev
// g++ nv12_gbm_egl.cpp -o nv12_gbm_egl -lEGL -lGLESv2 -lX11 -lgbm -ldrm
//
// Run (ensure test_nv12.yuv 640x480 exists and you have permission to /dev/dri/renderD128):
// ./nv12_gbm_egl
//
// Output: output.rgb (RGB24 raw)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

#include <X11/Xlib.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <gbm.h>
// #include <drm_fourcc.h>
#define DRM_FORMAT_NV12 0x3231564E  // 'NV12'
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 DRM_FORMAT_NV12
#endif

// EGL function pointers (some systems require eglGetProcAddress)
typedef EGLImageKHR (EGLAPIENTRYP PFNEGLCREATEIMAGEPROC)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYIMAGEPROC)(EGLDisplay dpy, EGLImageKHR image);
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, GLeglImageOES image);

static PFNEGLCREATEIMAGEPROC p_eglCreateImage = NULL;
static PFNEGLDESTROYIMAGEPROC p_eglDestroyImage = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES = NULL;

static int WIDTH = 640;
static int HEIGHT = 480;
static const char* NV12_FILE = "frame_nv12.raw";

static const char* vs_src =
"attribute vec2 aPos;\n"
"attribute vec2 aTex;\n"
"varying vec2 vTex;\n"
"void main(){ gl_Position = vec4(aPos,0.0,1.0); vTex = aTex; }\n";

static const char* fs_src =
"precision mediump float;\n"
"varying vec2 vTex;\n"
"uniform sampler2D texY;\n"
"uniform sampler2D texUV;\n"
"void main(){\n"
"   float y = texture2D(texY, vTex).r;\n" // Y in R
"   // NV12 UV is interleaved as (U,V). We upload as GL_LUMINANCE_ALPHA,\n"
"   // where L -> RGB, A -> alpha. So fetch U from .r (L) and V from .a (alpha).\n"
"   vec2 uv = texture2D(texUV, vTex).ra;\n" // U in .r (L), V in .a (alpha)
"   float u = uv.x - 0.5;\n"
"   float v = uv.y - 0.5;\n"
"   // Assume full-range YUV. If your source is limited-range, enable the offset/scale.\n"
"   float r = y + 1.402 * v;\n"
"   float g = y - 0.344136 * u - 0.714136 * v;\n"
"   float b = y + 1.772 * u;\n"
"   gl_FragColor = vec4(r,g,b,1.0);\n"
"}\n";

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
        fprintf(stderr, "shader compile error: %s\n", buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
static GLuint create_program(const char* vs, const char* fs) {
    GLuint sv = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint sf = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!sv || !sf) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, sv); glAttachShader(p, sf);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aTex");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetProgramInfoLog(p, sizeof(buf), NULL, buf);
        fprintf(stderr, "program link error: %s\n", buf);
        glDeleteProgram(p);
        return 0;
    }
    glDeleteShader(sv); glDeleteShader(sf);
    return p;
}

static int egl_has_ext(EGLDisplay dpy, const char* name) {
    const char* s = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!s) return 0;
    return strstr(s, name) != NULL;
}

int main() {
    // 1. read test NV12 file
    size_t y_size = WIDTH * HEIGHT;
    size_t uv_size = WIDTH * HEIGHT / 2;
    FILE* f = fopen(NV12_FILE, "rb");
    if (!f) {
        fprintf(stderr, "Failed open %s: %s\n", NV12_FILE, strerror(errno));
        return 1;
    }
    uint8_t* bufY = (uint8_t*)malloc(y_size);
    uint8_t* bufUV = (uint8_t*)malloc(uv_size);
    if (!bufY || !bufUV) { fprintf(stderr,"alloc failed\n"); return 1; }
    if (fread(bufY,1,y_size,f) != y_size || fread(bufUV,1,uv_size,f) != uv_size) {
        fprintf(stderr,"read nv12 failed or wrong file size\n"); return 1;
    }
    fclose(f);

    // 2. open DRM render node and create GBM device
    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open /dev/dri/renderD128");
        fprintf(stderr, "You need access to a render node (run on machine with GPU and permission)\n");
        // fallback: still continue but will not try GBM path
    }

    struct gbm_device* gbm = NULL;
    struct gbm_bo* bo = NULL;
    int gbm_export_fd = -1;
    uint32_t stride0=0, stride1=0;
    uint64_t offset0=0, offset1=0;
    int plane_count = 0;
    int use_gbm = 0;

    if (drm_fd >= 0) {
        gbm = gbm_create_device(drm_fd);
        if (!gbm) {
            fprintf(stderr,"gbm_create_device failed\n");
        } else {
            // create NV12 bo
            // flags: use rendering and linear to make mapping easier (driver may ignore)
            uint32_t flags = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR;
            bo = gbm_bo_create(gbm, WIDTH, HEIGHT, GBM_FORMAT_NV12, flags);
            if (!bo) {
                fprintf(stderr, "gbm_bo_create failed\n");
            } else {
                // try to export fd
                // gbm_bo_get_fd exists in modern libgbm
#ifdef GBM_BO_GET_FD
                gbm_export_fd = gbm_bo_get_fd(bo);
#else
                // fallback to gbm_bo_get_handle + drmPrimeHandleToFD (if available)
                union gbm_bo_handle handle = gbm_bo_get_handle(bo);
                int prime_fd = -1;
                if (handle.u32) {
                    // try DRM PRIME export
                    int ret = drmPrimeHandleToFD(drm_fd, handle.u32, DRM_CLOEXEC | DRM_RDWR, &prime_fd);
                    if (ret == 0) gbm_export_fd = prime_fd;
                }
#endif
                if (gbm_export_fd < 0) {
                    fprintf(stderr,"gbm export fd failed (gbm_export_fd=%d)\n", gbm_export_fd);
                } else {
                    // get plane info if available
#ifdef gbm_bo_get_plane_count
                    plane_count = gbm_bo_get_plane_count(bo);
#else
                    plane_count = 2; // assume NV12 two planes
#endif
                    // obtain stride and offset for planes
#ifdef gbm_bo_get_stride_for_plane
                    stride0 = gbm_bo_get_stride_for_plane(bo, 0);
                    stride1 = gbm_bo_get_stride_for_plane(bo, 1);
#else
                    stride0 = gbm_bo_get_stride(bo);
                    stride1 = stride0;
#endif
#ifdef gbm_bo_get_offset
                    offset0 = gbm_bo_get_offset(bo, 0);
                    offset1 = gbm_bo_get_offset(bo, 1);
#else
                    offset0 = 0;
                    offset1 = (uint64_t)(WIDTH * HEIGHT);
#endif
                    use_gbm = 1;
                    printf("GBM bo created: fd=%d planes=%d stride0=%u stride1=%u off0=%llu off1=%llu\n",
                           gbm_export_fd, plane_count, stride0, stride1, (unsigned long long)offset0, (unsigned long long)offset1);
                    // map dma-buf fd to write NV12 data
                    // compute total size conservatively: offset1 + stride1 * height/2
                    size_t total_sz = offset1 + stride1 * (HEIGHT/2);
                    void* mmap_ptr = mmap(NULL, total_sz, PROT_WRITE | PROT_READ, MAP_SHARED, gbm_export_fd, 0);
                    if (mmap_ptr == MAP_FAILED) {
                        perror("mmap gbm_export_fd");
                        fprintf(stderr,"Can't mmap exported gbm fd; falling back to CPU upload\n");
                        use_gbm = 0;
                    } else {
                        // copy Y at offset0, per-line using stride0
                        uint8_t* base = (uint8_t*)mmap_ptr;
                        for (int r=0; r<HEIGHT; ++r) {
                            memcpy(base + offset0 + r*stride0, bufY + r*WIDTH, WIDTH);
                        }
                        // UV plane: interleaved UV line length is WIDTH (bytes) but stride may differ
                        for (int r=0; r<HEIGHT/2; ++r) {
                            memcpy(base + offset1 + r*stride1, bufUV + r*(WIDTH), WIDTH);
                        }
                        msync(mmap_ptr, total_sz, MS_SYNC);
                        munmap(mmap_ptr, total_sz);
                        printf("Wrote NV12 into exported gbm dma-buf\n");
                    }
                }
            }
        }
    }

    // 3. Create X11 + EGL + GL context (pbuffer to be headless-render)
    Display* xdisp = XOpenDisplay(NULL);
    if (!xdisp) { fprintf(stderr,"XOpenDisplay failed. continuing headless? try setting DISPLAY\n"); return 1; }
    Window root = DefaultRootWindow(xdisp);
    XSetWindowAttributes swa; swa.event_mask = ExposureMask | KeyPressMask;
    Window win = XCreateWindow(xdisp, root, 0,0, WIDTH, HEIGHT, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &swa);
    XMapWindow(xdisp, win);
    XStoreName(xdisp, win, "NV12 GBM EGL Demo");
    Atom wmDeleteMessage = XInternAtom(xdisp, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(xdisp, win, &wmDeleteMessage, 1);

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)xdisp);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr,"eglGetDisplay failed\n"); return 1; }
    if (!eglInitialize(dpy, NULL, NULL)) { fprintf(stderr,"eglInitialize failed\n"); return 1; }

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint ncfg;
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &ncfg) || ncfg==0) { fprintf(stderr,"eglChooseConfig failed\n"); return 1; }
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, NULL);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (!ctx) { fprintf(stderr,"eglCreateContext failed\n"); return 1; }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr,"eglMakeCurrent failed\n"); return 1; }

    // load extension entrypoints
    p_eglCreateImage = (PFNEGLCREATEIMAGEPROC)eglGetProcAddress("eglCreateImage");
    p_eglDestroyImage = (PFNEGLDESTROYIMAGEPROC)eglGetProcAddress("eglDestroyImage");
    p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p_glEGLImageTargetTexture2DOES) {
        fprintf(stderr,"glEGLImageTargetTexture2DOES not available\n");
    }

    int have_dma_ext = egl_has_ext(dpy, "EGL_EXT_image_dma_buf_import");
    if (!have_dma_ext) {
        fprintf(stderr,"EGL_EXT_image_dma_buf_import NOT available -> will fallback to CPU GL upload\n");
    } else {
        printf("EGL_EXT_image_dma_buf_import available\n");
    }

    // 4. Try to create EGLImage from GBM-exported fd (NV12)
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    int imported = 0;
    if (use_gbm && have_dma_ext && p_eglCreateImage && p_glEGLImageTargetTexture2DOES) {
        // Use EGLAttrib array for modern eglCreateImage signature
        EGLAttrib attr[] = {
            EGL_WIDTH, WIDTH,
            EGL_HEIGHT, HEIGHT,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
            EGL_DMA_BUF_PLANE0_FD_EXT, gbm_export_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLAttrib)offset0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)stride0,
            EGL_DMA_BUF_PLANE1_FD_EXT, gbm_export_fd,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLAttrib)offset1,
            EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLAttrib)stride1,
            EGL_NONE
        };
        img = p_eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attr);
        if (img == EGL_NO_IMAGE_KHR) {
            EGLint err = eglGetError();
            fprintf(stderr, "eglCreateImage(dma_buf) failed: 0x%04x\n", err);
        } else {
            // bind to texture
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
            GLenum gerr = glGetError();
            if (gerr != GL_NO_ERROR) {
                fprintf(stderr, "glEGLImageTargetTexture2DOES failed: 0x%x\n", gerr);
                // destroy and fallback
                p_eglDestroyImage(dpy, img);
                img = EGL_NO_IMAGE_KHR;
            } else {
                printf("Imported EGLImage from GBM-exported dma-buf and bound to texture %u\n", tex);
                imported = 1;
            }
        }
    }

    // create shader program
    GLuint prog = create_program(vs_src, fs_src);
    if (!prog) { fprintf(stderr,"create_program failed\n"); return 1; }
    glUseProgram(prog);
    GLint locY = glGetUniformLocation(prog, "texY");
    GLint locUV = glGetUniformLocation(prog, "texUV");
    glUniform1i(locY, 0);
    glUniform1i(locUV, 1);

    // vertex data
    float verts[] = {
        -1.f,-1.f, 0.f,1.f,
         1.f,-1.f, 1.f,1.f,
        -1.f, 1.f, 0.f,0.f,
         1.f, 1.f, 1.f,0.f
    };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float))); glEnableVertexAttribArray(1);

    GLuint texY = 0, texUV = 0;
    if (!imported) {
        // fallback: create two 2D textures from bufY / bufUV
        glGenTextures(1, &texY);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texY);
        // Use GL_RED or GL_LUMINANCE depending on driver; GL_RED may not available in GLES2, but GL_LUMINANCE is widely available.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, WIDTH, HEIGHT, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, bufY);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &texUV);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texUV);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, WIDTH/2, HEIGHT/2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, bufUV);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        printf("Fallback: uploaded Y/UV to GL textures\n");
    } else {
        // imported path: we bound single EGLImage to a texture earlier (tex is currently bound). Some drivers expose planar sampling differently.
        // For safety, create a duplicate GL texture unit binding for sampling as texY (the shader expects two textures).
        // Simpler approach: sample the imported NV12 texture as GL_TEXTURE_2D into both samplers; driver-dependent behavior.
        // We'll bind the same texture to both units.
        GLuint boundTex; glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint*)&boundTex);
        texY = boundTex;
        texUV = boundTex;
    }

    // free cpu buffers
    free(bufY); free(bufUV);

    // initial render + readback once
    glViewport(0,0, WIDTH, HEIGHT);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(dpy, surf);

    uint8_t* out = (uint8_t*)malloc(WIDTH*HEIGHT*3);
    glReadPixels(0,0, WIDTH, HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, out);
    FILE* fout = fopen("output.rgb","wb");
    if (fout) { fwrite(out,1,WIDTH*HEIGHT*3,fout); fclose(fout); printf("Wrote output.rgb\n"); }
    free(out);

    // Keep window open and continue swapping until user closes
    int running = 1;
    while (running) {
        // Simple redraw
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        eglSwapBuffers(dpy, surf);

        // Handle X events (close on WM_DELETE or key press 'q'/'Esc')
        while (XPending(xdisp)) {
            XEvent ev; XNextEvent(xdisp, &ev);
            if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == wmDeleteMessage) {
                    running = 0;
                }
            } else if (ev.type == KeyPress) {
                running = 0;
            }
        }
        usleep(16000); // ~60 FPS throttle
    }

    // cleanup
    if (img != EGL_NO_IMAGE_KHR && p_eglDestroyImage) p_eglDestroyImage(dpy, img);
    if (use_gbm) {
        if (gbm && bo) {
            gbm_bo_destroy(bo);
            gbm_device_destroy(gbm);
        }
        if (gbm_export_fd >= 0) close(gbm_export_fd);
        if (drm_fd >= 0) close(drm_fd);
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    XDestroyWindow(xdisp, win);
    XCloseDisplay(xdisp);

    printf("Done.\n");
    return 0;
}
