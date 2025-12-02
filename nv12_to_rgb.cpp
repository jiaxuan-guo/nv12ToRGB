#include <iostream>
#include <fstream>
#include <vector>

// NV12是YUV420格式，Y平面后接UV交织平面
// 输入:
//   nv12_data: NV12格式数据缓冲区
//   width, height: 图像宽高
// 输出:
//   rgb_data: 输出的RGB24数据缓冲区（width * height * 3字节）
void NV12ToRGB(const uint8_t* nv12_data, int width, int height, std::vector<uint8_t>& rgb_data) {
    rgb_data.resize(width * height * 3);
    const uint8_t* y_plane = nv12_data;
    const uint8_t* uv_plane = nv12_data + width * height;

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int y_index = j * width + i;
            int uv_index = (j / 2) * (width) + (i & ~1);

            int Y = y_plane[y_index];
            int U = uv_plane[uv_index] - 128;
            int V = uv_plane[uv_index + 1] - 128;

            // YUV to RGB conversion (BT.601)
            int C = Y - 16;
            int D = U;
            int E = V;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            R = std::min(std::max(R, 0), 255);
            G = std::min(std::max(G, 0), 255);
            B = std::min(std::max(B, 0), 255);

            int rgb_index = y_index * 3;
            rgb_data[rgb_index + 0] = static_cast<uint8_t>(R);
            rgb_data[rgb_index + 1] = static_cast<uint8_t>(G);
            rgb_data[rgb_index + 2] = static_cast<uint8_t>(B);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cout << "Usage: " << argv[0] << " input_nv12_file width height\n";
        return -1;
    }

    const char* input_file = argv[1];
    int width = atoi(argv[2]);
    int height = atoi(argv[3]);

    // 计算NV12数据大小
    size_t nv12_size = width * height * 3 / 2;

    std::vector<uint8_t> nv12_data(nv12_size);
    std::ifstream fin(input_file, std::ios::binary);
    if (!fin) {
        std::cerr << "Failed to open input file\n";
        return -1;
    }

    fin.read(reinterpret_cast<char*>(nv12_data.data()), nv12_size);
    if (!fin) {
        std::cerr << "Failed to read full NV12 data\n";
        return -1;
    }
    fin.close();

    std::vector<uint8_t> rgb_data;
    NV12ToRGB(nv12_data.data(), width, height, rgb_data);

    // 输出RGB到文件
    std::ofstream fout("output.rgb", std::ios::binary);
    fout.write(reinterpret_cast<const char*>(rgb_data.data()), rgb_data.size());
    fout.close();

    std::cout << "Conversion done, output.rgb generated (" << rgb_data.size() << " bytes)\n";
    return 0;
}

