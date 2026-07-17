#ifndef MD5_H_
#define MD5_H_

#include <cstdint>
#include <cstddef>
#include <string>

/**
 * MD5 哈希算法工具类
 * 用于计算本地文件的 MD5，与服务器端的 mjpeg_hash 比对
 * 从而决定是否需要下载/覆盖
 */
class MD5 {
public:
    /**
     * 计算文件 MD5
     * @param file_path 文件绝对路径
     * @return 32 位小写 hex 字符串，失败返回空字符串
     */
    static std::string Calculate(const std::string& file_path);

    /**
     * 计算内存数据 MD5
     * @param data 数据指针
     * @param len 数据长度
     * @return 32 位小写 hex 字符串
     */
    static std::string Calculate(const uint8_t* data, size_t len);

private:
    struct Context {
        uint32_t state[4];
        uint32_t count[2];
        uint8_t buffer[64];
    };

    static void Init(Context* ctx);
    static void Update(Context* ctx, const uint8_t* input, size_t inputLen);
    static void Final(uint8_t digest[16], Context* ctx);
    static void Transform(uint32_t state[4], const uint8_t block[64]);
    static std::string ToHexString(const uint8_t digest[16]);
};

#endif // MD5_H_
