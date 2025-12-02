#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

#define WIDTH 640
#define HEIGHT 480

// 简单顶点着色器
const char* vertexShaderSource = R"(
    #version 300 es
    layout(location = 0) in vec4 a_position;
    layout(location = 1) in vec2 a_texCoord;
    out vec2 v_texCoord;
    void main() {
        gl_Position = a_position;
        v_texCoord = a_texCoord;
    }
)";

// 片元着色器：NV12转RGB
const char* fragmentShaderSource = R"(
    #version 300 es
    precision mediump float;
    in vec2 v_texCoord;
    layout(location = 0) out vec4 outColor;
    uniform sampler2D texY;
    uniform sampler2D texUV;
    void main() {
        float y = texture(texY, v_texCoord).r;
        vec2 uv = texture(texUV, v_texCoord).rg;
        float u = uv.x - 0.5;
        float v = uv.y - 0.5;
        float r = y + 1.402 * v;
        float g = y - 0.344136 * u - 0.714136 * v;
        float b = y + 1.772 * u;
        outColor = vec4(r, g, b, 1.0);
    }
)";

// 创建并编译着色器
GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char info[512];
        glGetShaderInfoLog(shader, 512, NULL, info);
        printf("Shader compile failed: %s\n", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// 创建着色器程序
GLuint createProgram() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char info[512];
        glGetProgramInfoLog(prog, 512, NULL, info);
        printf("Program link failed: %s\n", info);
        glDeleteProgram(prog);
        return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

int main() {
    // 1. 初始化X11显示
    Display* x_display = XOpenDisplay(NULL);
    if (!x_display) {
        printf("Cannot open X display\n");
        return -1;
    }
    Window root = DefaultRootWindow(x_display);

    // 创建简单窗口
    XSetWindowAttributes swa;
    swa.event_mask = ExposureMask | PointerMotionMask | KeyPressMask;
    Window win = XCreateWindow(
        x_display, root, 0, 0, WIDTH, HEIGHT, 0,
        CopyFromParent, InputOutput,
        CopyFromParent, CWEventMask, &swa);
    XMapWindow(x_display, win);
    XStoreName(x_display, win, "NV12 to RGB OpenGL ES Demo");

    // 2. 初始化EGL
    EGLDisplay eglDisplay = eglGetDisplay((EGLNativeDisplayType)x_display);
    if (eglDisplay == EGL_NO_DISPLAY) {
        printf("eglGetDisplay failed\n");
        return -1;
    }
    if (!eglInitialize(eglDisplay, NULL, NULL)) {
        printf("eglInitialize failed\n");
        return -1;
    }

    // 3. 选择EGL配置
    EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) {
        printf("eglChooseConfig failed\n");
        return -1;
    }

    // 4. 创建EGL窗口表面
    EGLSurface eglSurface = eglCreateWindowSurface(eglDisplay, config, (EGLNativeWindowType)win, NULL);
    if (eglSurface == EGL_NO_SURFACE) {
        printf("eglCreateWindowSurface failed\n");
        return -1;
    }

    // 5. 创建OpenGL ES 3.0上下文
    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext eglContext = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext == EGL_NO_CONTEXT) {
        printf("eglCreateContext failed\n");
        return -1;
    }

    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        printf("eglMakeCurrent failed\n");
        return -1;
    }

    // 6. 读取NV12文件
    FILE* f = fopen("frame_nv12.raw", "rb");
    if (!f) {
        printf("Failed to open test_nv12.yuv\n");
        return -1;
    }
    unsigned char* dataY = (unsigned char*)malloc(WIDTH * HEIGHT);
    unsigned char* dataUV = (unsigned char*)malloc(WIDTH * HEIGHT / 2);
    size_t readY = fread(dataY, 1, WIDTH * HEIGHT, f);
    size_t readUV = fread(dataUV, 1, WIDTH * HEIGHT / 2, f);
    fclose(f);
    if (readY != WIDTH * HEIGHT || readUV != WIDTH * HEIGHT / 2) {
        printf("Failed to read enough NV12 data\n");
        free(dataY);
        free(dataUV);
        return -1;
    }

    // 7. 创建着色器程序
    GLuint program = createProgram();
    if (!program) return -1;
    glUseProgram(program);

    // 获取纹理单元uniform位置
    GLint locY = glGetUniformLocation(program, "texY");
    GLint locUV = glGetUniformLocation(program, "texUV");
    glUniform1i(locY, 0);
    glUniform1i(locUV, 1);

    // 8. 创建两个纹理：Y和UV
    GLuint texY, texUV;
    glGenTextures(1, &texY);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, WIDTH, HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, dataY);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenTextures(1, &texUV);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texUV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, WIDTH / 2, HEIGHT / 2, 0, GL_RG, GL_UNSIGNED_BYTE, dataUV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    free(dataY);
    free(dataUV);

    // 9. 准备顶点数据（两个三角形覆盖整个屏幕）
    float vertices[] = {
        // 位置       // 纹理坐标
        -1.f, -1.f,   0.f, 1.f,
         1.f, -1.f,   1.f, 1.f,
        -1.f,  1.f,   0.f, 0.f,
         1.f,  1.f,   1.f, 0.f,
    };

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // a_position (location = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    // a_texCoord (location = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    // 10. 渲染循环（简单版，只渲染一次）
    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(eglDisplay, eglSurface);

    // 保持窗口，等待用户关闭
    XEvent event;
    bool quit = false;
    while (!quit) {
        XNextEvent(x_display, &event);
        if (event.type == KeyPress) {
            quit = true;
        }
    }

    // 清理资源
    glDeleteTextures(1, &texY);
    glDeleteTextures(1, &texUV);
    glDeleteProgram(program);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    eglDestroyContext(eglDisplay, eglContext);
    eglDestroySurface(eglDisplay, eglSurface);
    eglTerminate(eglDisplay);
    XDestroyWindow(x_display, win);
    XCloseDisplay(x_display);

    return 0;
}

