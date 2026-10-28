import Foundation
import CryptoKit
import Security

// C ABI only. Buffers belong to the bridge until ugos_release(). No input
// values appear in errors; no key material is persisted or sent to a process.
private func data(_ pointer: UnsafePointer<UInt8>?, _ count: Int32) -> Data? {
    guard count >= 0, count == 0 || pointer != nil else { return nil }
    return count == 0 ? Data() : Data(bytes: pointer!, count: Int(count))
}
private func emit(_ value: Data, _ output: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?,
                  _ count: UnsafeMutablePointer<Int32>?) -> Int32 {
    guard let output = output, let count = count, value.count <= Int(Int32.max) else { return 0 }
    let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: max(1,value.count))
    value.copyBytes(to: buffer, count: value.count)
    output.pointee = buffer
    count.pointee = Int32(value.count)
    return 1
}
@_cdecl("ugos_release")
public func releaseBytes(_ pointer: UnsafeMutablePointer<UInt8>?, _ count: Int32) {
    guard let pointer = pointer else { return }
    pointer.update(repeating: 0, count: max(0,Int(count)))
    pointer.deallocate()
}
@_cdecl("ugos_random")
public func randomBytes(_ output: UnsafeMutablePointer<UInt8>?, _ count: Int32) -> Int32 {
    guard count > 0, let output = output else { return 0 }
    return SecRandomCopyBytes(kSecRandomDefault,Int(count),output) == errSecSuccess ? 1 : 0
}
@_cdecl("ugos_seal")
public func seal(_ key: UnsafePointer<UInt8>?, _ keyLength: Int32,
                 _ nonce: UnsafePointer<UInt8>?, _ nonceLength: Int32,
                 _ plain: UnsafePointer<UInt8>?, _ plainLength: Int32,
                 _ output: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?,
                 _ outputLength: UnsafeMutablePointer<Int32>?) -> Int32 {
    guard keyLength == 32, nonceLength == 12,
          let key = data(key,keyLength), let nonce = data(nonce,nonceLength),
          let plain = data(plain,plainLength) else { return 0 }
    do {
        let box = try AES.GCM.seal(plain,using: SymmetricKey(data:key),
                                   nonce: AES.GCM.Nonce(data:nonce))
        guard let combined = box.combined else { return 0 }
        return emit(combined,output,outputLength)
    } catch { return 0 }
}
@_cdecl("ugos_open")
public func openBox(_ key: UnsafePointer<UInt8>?, _ keyLength: Int32,
                    _ combined: UnsafePointer<UInt8>?, _ combinedLength: Int32,
                    _ output: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?,
                    _ outputLength: UnsafeMutablePointer<Int32>?) -> Int32 {
    guard keyLength == 32, combinedLength >= 28, let key = data(key,keyLength),
          let combined = data(combined,combinedLength) else { return 0 }
    do {
        let box = try AES.GCM.SealedBox(combined:combined)
        return emit(try AES.GCM.open(box,using:SymmetricKey(data:key)),output,outputLength)
    } catch { return 0 }
}
// Security's RSA importer expects PKCS#1. Unwrap SPKI, when present.
private func rsaDer(_ pem: Data) -> Data? {
    guard let text = String(data:pem,encoding:.utf8) else { return nil }
    let encoded = text.components(separatedBy:.newlines)
        .filter { !$0.hasPrefix("-----") }.joined()
    guard let der = Data(base64Encoded:encoded), der.count > 8 else { return nil }
    let bytes = [UInt8](der)
    func element(_ start: Int) -> (tag:UInt8, content:Int, end:Int)? {
        guard start >= 0, start+2 <= bytes.count else { return nil }
        var length = Int(bytes[start+1]), content = start+2
        if length & 0x80 != 0 {
            let width = length & 0x7f
            guard width > 0, width <= 4, content+width <= bytes.count else { return nil }
            length = 0
            for byte in bytes[content..<content+width] { length = (length << 8) | Int(byte) }
            content += width
        }
        guard length <= bytes.count-content else { return nil }
        return (bytes[start],content,content+length)
    }
    guard let outer = element(0), outer.tag == 0x30, outer.end == bytes.count,
          let first = element(outer.content) else { return nil }
    if first.tag == 0x02 { return der }
    guard first.tag == 0x30, let bits = element(first.end), bits.tag == 0x03,
          bits.end == outer.end, bits.content < bits.end, bytes[bits.content] == 0 else { return nil }
    return Data(bytes[(bits.content+1)..<bits.end])
}
@_cdecl("ugos_rsa_encrypt")
public func rsaEncrypt(_ pem: UnsafePointer<UInt8>?, _ pemLength: Int32,
                       _ plain: UnsafePointer<UInt8>?, _ plainLength: Int32,
                       _ output: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?,
                       _ outputLength: UnsafeMutablePointer<Int32>?) -> Int32 {
    guard pemLength > 0, pemLength <= 16384, plainLength >= 0, plainLength <= 8192,
          let pem = data(pem,pemLength), let der = rsaDer(pem),
          let plain = data(plain,plainLength) else { return 0 }
    let attributes: [CFString:Any] = [kSecAttrKeyType:kSecAttrKeyTypeRSA,
                                     kSecAttrKeyClass:kSecAttrKeyClassPublic]
    guard let key = SecKeyCreateWithData(der as CFData,attributes as CFDictionary,nil),
          SecKeyIsAlgorithmSupported(key,.encrypt,.rsaEncryptionPKCS1),
          let encrypted = SecKeyCreateEncryptedData(key,.rsaEncryptionPKCS1,plain as CFData,nil)
          else { return 0 }
    return emit(encrypted as Data,output,outputLength)
}
