import CTetherKitNext
import Foundation

// C's fixed-size char arrays are imported into Swift as **tuples** (`char[16]` -> a tuple of 16 CChars),
// which can be neither subscripted nor converted directly to and from String. This file handles that in one place,
// avoiding writing unsafe pointer operations again for every field.

extension String {
    /// Reads a string from a C fixed-size char array (imported as a tuple).
    ///
    /// `String(cString:)` is not used: it requires the buffer to be NUL-terminated, and once the C side fills the entire
    /// buffer it would read out of bounds. This is changed to "scan to a NUL or to the end", which is safe in both cases.
    init<T>(fixedCArray tuple: T) {
        var mutableCopy = tuple
        self = withUnsafeBytes(of: &mutableCopy) { raw in
            let bytes = raw.prefix { $0 != 0 }
            return String(decoding: bytes, as: UTF8.self)
        }
    }
}

/// Writes a string into a C fixed-size char array, guaranteeing NUL termination.
///
/// Truncates when too long, and **the truncation lands on a UTF-8 character boundary** -- cutting hard by bytes would leave
/// half a character at the end of the buffer, which the C side would then read out as a string of replacement characters. (The C side's CopyText has the same handling,
/// and both directions need care.)
func setFixedCArray<T>(_ tuple: inout T, to string: String) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        writeCString(string, into: raw)
    }
}

/// Writes a string into a raw buffer, NUL-terminated + UTF-8-boundary-safe truncation.
private func writeCString(_ string: String, into raw: UnsafeMutableRawBufferPointer) {
    guard raw.count > 0 else { return }
    for index in raw.indices { raw[index] = 0 }

    let utf8 = Array(string.utf8)
    var length = min(utf8.count, raw.count - 1)
    // length < utf8.count means truncation happened; if the cut point falls in the middle of a multi-byte sequence (the top two bits of a continuation byte
    // are 0b10), back up all the way to the start of that sequence.
    while length > 0, length < utf8.count, utf8[length] & 0xC0 == 0x80 {
        length -= 1
    }
    for index in 0..<length {
        raw[index] = utf8[index]
    }
}

/// Writes a string into row `row` of a C two-dimensional fixed-size array (such as `char dns[4][46]`).
///
/// A two-dimensional array in Swift is a "tuple of tuples" that cannot be subscripted and can only be located by byte offset.
func setFixedCArrayRow<T>(_ tuple: inout T, row: Int, stride: Int, to string: String) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        let start = row * stride
        guard start >= 0, start + stride <= raw.count else { return }
        writeCString(string, into: UnsafeMutableRawBufferPointer(rebasing: raw[start..<(start + stride)]))
    }
}

/// Reads row `row` of a C two-dimensional fixed-size array.
func fixedCArrayRow<T>(_ tuple: T, row: Int, stride: Int) -> String {
    var mutableCopy = tuple
    return withUnsafeBytes(of: &mutableCopy) { raw in
        let start = row * stride
        guard start >= 0, start + stride <= raw.count else { return "" }
        let slice = raw[start..<(start + stride)].prefix { $0 != 0 }
        return String(decoding: slice, as: UTF8.self)
    }
}

/// Renders a 6-byte MAC tuple as "aa:bb:cc:dd:ee:ff"; returns an empty string when all 0.
func formatMAC<T>(_ tuple: T) -> String {
    var mutableCopy = tuple
    return withUnsafeBytes(of: &mutableCopy) { raw in
        guard raw.count >= 6, raw.prefix(6).contains(where: { $0 != 0 }) else { return "" }
        return raw.prefix(6).map { String(format: "%02x", $0) }.joined(separator: ":")
    }
}
