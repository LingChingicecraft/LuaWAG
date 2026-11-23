// =============================================================================
//  probes/cff_meta.cpp —— 只看元数据：这个字体是不是 CFF？是不是 CID-keyed？
//
//  用法：cff_meta <font.otf>
//
//  为什么单独写：CFF 的 Top DICT 里"ROS"（12 30）是 CID-keyed 的标志，
//  而普通 grep 找不到（字符串在 CFF 的 String INDEX 里，不是明文）。
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::uint16_t be16(const unsigned char* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

/// CFF INDEX：返回 [data_begin, data_end) 与每项偏移。解析失败返回 false。
struct index_view {
    std::uint32_t count = 0;
    std::uint32_t data_begin = 0;    // INDEX 数据区起点（相对 CFF 表头）
    std::uint32_t data_end = 0;
    std::vector<std::uint32_t> offsets; // count+1 个（相对 data_begin）
    std::uint32_t off_size = 1;
};

bool read_index(const unsigned char* cff, std::uint32_t cff_len, std::uint32_t at,
                index_view& out) {
    if (at + 2 > cff_len) return false;
    out.count = be16(cff + at);
    if (out.count == 0) {
        out.data_begin = at + 2;
        out.data_end = at + 2;
        return true;
    }
    if (at + 3 > cff_len) return false;
    out.off_size = cff[at + 2];
    if (out.off_size < 1 || out.off_size > 4) return false;
    const std::uint32_t offs_at = at + 3;
    if (offs_at + (out.count + 1) * out.off_size > cff_len) return false;
    out.offsets.resize(out.count + 1);
    for (std::uint32_t i = 0; i <= out.count; ++i) {
        std::uint32_t v = 0;
        for (std::uint32_t b = 0; b < out.off_size; ++b) {
            v = (v << 8) | cff[offs_at + i * out.off_size + b];
        }
        out.offsets[i] = v;
    }
    out.data_begin = offs_at + (out.count + 1) * out.off_size - 1; // CFF 偏移以 1 为基
    out.data_end = out.data_begin + out.offsets.back();
    return out.data_end <= cff_len;
}

/// 在一个 DICT 里找操作符 12 30（ROS）。
bool dict_has_ros(const unsigned char* p, std::uint32_t len) {
    std::uint32_t i = 0;
    while (i < len) {
        const unsigned char b0 = p[i];
        if (b0 <= 21) {
            if (b0 == 12 && i + 1 < len && p[i + 1] == 30) {
                return true;
            }
            i += (b0 == 12) ? 2 : 1;
        } else if (b0 == 28) {
            i += 3;
        } else if (b0 == 29) {
            i += 5;
        } else if (b0 == 30) {
            // real number：以 0xF 结尾的半字节
            ++i;
            while (i < len) {
                const unsigned char b = p[i++];
                if ((b & 0x0F) == 0x0F || (b >> 4) == 0x0F) break;
            }
        } else if (b0 >= 32 && b0 <= 246) {
            ++i;
        } else if (b0 >= 247 && b0 <= 250) {
            i += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            i += 2;
        } else {
            ++i; // 未知：跳过
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: cff_meta <font.otf>\n");
        return 2;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (f == nullptr) {
        std::printf("读不到: %s\n", argv[1]);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size() || bytes.size() < 12) {
        std::printf("文件太小或读不完整\n");
        return 1;
    }

    const std::uint32_t tag = be32(bytes.data());
    const char* tag_s = (tag == 0x4F54544Fu)   ? "OTTO（CFF 轮廓）"
                        : (tag == 0x00010000u) ? "0x00010000（TrueType 轮廓）"
                        : (tag == 0x74746366u) ? "ttcf（字体集合）"
                                               : "未知";
    std::printf("文件：%s\n", argv[1]);
    std::printf("  sfnt：%s\n", tag_s);

    const std::uint16_t n = be16(bytes.data() + 4);
    std::uint32_t cff_off = 0;
    std::uint32_t cff_len = 0;
    bool has_glyf = false;
    bool has_cff2 = false;
    std::uint32_t cff2_len = 0;
    std::printf("  表目录：\n");
    for (std::uint16_t i = 0; i < n; ++i) {
        const unsigned char* rec = bytes.data() + 12 + 16ull * i;
        const std::uint32_t off = be32(rec + 8);
        const std::uint32_t len = be32(rec + 12);
        std::printf("    %.4s  off=%-9u len=%u\n", reinterpret_cast<const char*>(rec), off, len);
        if (std::memcmp(rec, "CFF ", 4) == 0) {
            cff_off = off;
            cff_len = len;
        }
        if (std::memcmp(rec, "CFF2", 4) == 0) {
            has_cff2 = true;
            cff2_len = len;
        }
        if (std::memcmp(rec, "glyf", 4) == 0) has_glyf = true;
    }
    std::printf("  小结：glyf=%s；CFF(1)=%s（%u 字节）；CFF2=%s（%u 字节）\n",
                has_glyf ? "有" : "无", cff_len ? "有" : "无", cff_len,
                has_cff2 ? "有" : "无", cff2_len);
    if (cff_len == 0) {
        std::printf("  没有 `CFF ` 表%s\n", has_cff2 ? "（只有 CFF2 ⇒ 这是 CFF2/可变字体轮廓）" : "");
        return 0;
    }
    if (cff_off + cff_len > bytes.size()) {
        std::printf("  CFF 表偏移越界，结束\n");
        return 0;
    }

    const unsigned char* cff = bytes.data() + cff_off;
    const unsigned char major = cff[0];
    const unsigned char minor = cff[1];
    const unsigned char hdr = cff[2];
    std::printf("  CFF 版本：%u.%u（hdrSize=%u）\n", major, minor, hdr);

    index_view names{};
    if (!read_index(cff, cff_len, hdr, names)) {
        std::printf("  Name INDEX 解析失败\n");
        return 1;
    }
    std::printf("  Name INDEX：%u 项\n", names.count);

    index_view tops{};
    // Top DICT INDEX 紧跟在 Name INDEX 之后
    if (!read_index(cff, cff_len, names.data_end, tops) || tops.count == 0) {
        std::printf("  Top DICT INDEX 解析失败\n");
        return 1;
    }
    const std::uint32_t d0 = tops.data_begin + tops.offsets[0];
    const std::uint32_t d1 = tops.data_begin + tops.offsets[1];
    const bool cid = dict_has_ros(cff + d0, d1 - d0);
    std::printf("  Top DICT：#0 长度 %u 字节；ROS=%s ⇒ **%s**\n", d1 - d0,
                cid ? "有" : "无", cid ? "CID-keyed CFF" : "普通 CFF（非 CID）");
    return 0;
}
