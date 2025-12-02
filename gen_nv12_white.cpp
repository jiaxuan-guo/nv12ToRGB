#include <cstdio>
#include <cstdlib>

int main() {
    const int width = 640;
    const int height = 480;

    FILE* fp = fopen("test_nv12_white.yuv", "wb");
    if (!fp) {
        perror("Failed to open file");
        return EXIT_FAILURE;
    }

    // 分配内存
    unsigned char* yPlane = new unsigned char[width * height];
    unsigned char* uvPlane = new unsigned char[(width * height) / 2]; // UV是Y的一半大小

    // Y平面全255 (白色亮度)
    for (int i = 0; i < width * height; ++i) {
        yPlane[i] = 255;
    }

    // UV平面全128 (中性色度)
    for (int i = 0; i < (width * height) / 2; ++i) {
        uvPlane[i] = 128;
    }

    // 写Y平面
    fwrite(yPlane, 1, width * height, fp);
    // 写UV平面
    fwrite(uvPlane, 1, (width * height) / 2, fp);

    fclose(fp);
    delete[] yPlane;
    delete[] uvPlane;

    printf("Generated test_nv12_white.yuv (%dx%d) with white frame\n", width, height);
    return 0;
}
