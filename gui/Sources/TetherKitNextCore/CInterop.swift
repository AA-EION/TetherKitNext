import CTetherKitNext
import Foundation

// C 的定长 char 数组在 Swift 里被导入成**元组**（`char[16]` → 16 个 CChar 的
// 元组），既不能下标访问也不能直接和 String 互转。这里集中处理这件事，
// 避免每个字段都写一遍不安全的指针操作。

extension String {
    /// 从 C 的定长 char 数组（导入为元组）读出字符串。
    ///
    /// 不用 `String(cString:)`：那要求缓冲一定有 NUL 结尾，一旦 C 侧写满了整个
    /// 缓冲就会越界读。这里改成「扫到 NUL 或扫到底」，两种情况都安全。
    init<T>(fixedCArray tuple: T) {
        var mutableCopy = tuple
        self = withUnsafeBytes(of: &mutableCopy) { raw in
            let bytes = raw.prefix { $0 != 0 }
            return String(decoding: bytes, as: UTF8.self)
        }
    }
}

/// 把字符串写进 C 的定长 char 数组，保证 NUL 结尾。
///
/// 超长时截断，且**截断落在 UTF-8 字符边界上** —— 按字节硬切会在缓冲末尾留下
/// 半个字符，C 侧再读出来就是一串替换字符。（C 侧的 CopyText 有同样的处理，
/// 两个方向都得管。）
func setFixedCArray<T>(_ tuple: inout T, to string: String) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        writeCString(string, into: raw)
    }
}

/// 把字符串写进一段原始缓冲，NUL 结尾 + UTF-8 边界安全截断。
private func writeCString(_ string: String, into raw: UnsafeMutableRawBufferPointer) {
    guard raw.count > 0 else { return }
    for index in raw.indices { raw[index] = 0 }

    let utf8 = Array(string.utf8)
    var length = min(utf8.count, raw.count - 1)
    // length < utf8.count 说明截断了；若切点落在多字节序列中间（续接字节的高两位
    // 是 0b10），一路退到该序列的起点。
    while length > 0, length < utf8.count, utf8[length] & 0xC0 == 0x80 {
        length -= 1
    }
    for index in 0..<length {
        raw[index] = utf8[index]
    }
}

/// 把字符串写进 C 的二维定长数组的第 row 行（如 `char dns[4][46]`）。
///
/// 二维数组在 Swift 里是「元组的元组」，没法用下标，只能按字节偏移定位。
func setFixedCArrayRow<T>(_ tuple: inout T, row: Int, stride: Int, to string: String) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        let start = row * stride
        guard start >= 0, start + stride <= raw.count else { return }
        writeCString(string, into: UnsafeMutableRawBufferPointer(rebasing: raw[start..<(start + stride)]))
    }
}

/// 读 C 的二维定长数组的第 row 行。
func fixedCArrayRow<T>(_ tuple: T, row: Int, stride: Int) -> String {
    var mutableCopy = tuple
    return withUnsafeBytes(of: &mutableCopy) { raw in
        let start = row * stride
        guard start >= 0, start + stride <= raw.count else { return "" }
        let slice = raw[start..<(start + stride)].prefix { $0 != 0 }
        return String(decoding: slice, as: UTF8.self)
    }
}

/// 把 6 字节 MAC 元组渲染成 "aa:bb:cc:dd:ee:ff"；全 0 时返回空串。
func formatMAC<T>(_ tuple: T) -> String {
    var mutableCopy = tuple
    return withUnsafeBytes(of: &mutableCopy) { raw in
        guard raw.count >= 6, raw.prefix(6).contains(where: { $0 != 0 }) else { return "" }
        return raw.prefix(6).map { String(format: "%02x", $0) }.joined(separator: ":")
    }
}
