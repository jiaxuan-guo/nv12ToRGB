#include <GLES2/gl2.h>
#include <stdio.h>

int main() {
    const GLubyte* version = glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("GLSL ES Version: %s\n", version);
    return 0;
}

