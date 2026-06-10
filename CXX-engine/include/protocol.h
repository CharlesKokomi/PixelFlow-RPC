#include <cstdint>

//结构体按 1 字节对齐
#pragma pack(push, 1)
struct RpcHeader {
    uint32_t magic_number; // 魔数，用来过滤非法包
    uint32_t version;      // 版本号
    uint32_t body_len;     // 后面跟Body数据的长度
    uint32_t type;         // 业务类型
};
#pragma pack(pop)