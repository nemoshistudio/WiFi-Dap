/**
 * 目标烧录固件格式转换（纯 TS，无第三方依赖）。
 *
 * 设备端点 `POST /api/target/flash` 始终接收“从目标 Flash 基地址开始的裸镜像”
 * （octet-stream + X-Target-SHA256 为镜像的 SHA-256）。本模块在浏览器端把
 * `.bin` / `.elf|.out` / `.hex|.ihex` / `.srec|.s19|.mot` 统一转换成该镜像：
 *
 *  - BIN：原样；
 *  - ELF32 LE ARM：只取 PT_LOAD 中 LMA（p_paddr，回退 p_vaddr）落在
 *    [flashBase, flashBase+flashSize) 的段，忽略 RAM-only 段（.bss/.heap 等）；
 *  - Intel HEX：支持记录类型 00/01/02/03/04/05，校验和验证；
 *  - Motorola S-Record：S1/S2/S3 数据 + S0/S5/S6/S7/S8/S9 头/计数/启动记录。
 *
 * 镜像 = 从 flashBase 到最高数据地址，未覆盖的字节填 0xFF，并统计填充字节数。
 * 转换失败抛出 `FirmwareImageError`（技术描述，页面用 i18n 模板包装显示）。
 */

export type FirmwareFormat = 'bin' | 'elf' | 'hex' | 'srec'

export class FirmwareImageError extends Error {
    constructor(message: string) {
        super(message)
        this.name = 'FirmwareImageError'
    }
}

export interface FirmwareSegment {
    /** Flash 内绝对地址 */
    address: number
    /** 段字节数 */
    size: number
}

export interface FirmwareImage {
    format: FirmwareFormat
    /** 待发送给设备的裸镜像（base 起，0xFF 填充） */
    image: Uint8Array
    /** 镜像基地址（= 目标 flash_base） */
    base: number
    /** 源文件大小（字节） */
    sourceSize: number
    /** 参与烧录的段（ELF 为 PT_LOAD 段；HEX/SREC/BIN 为数据块合并信息） */
    segments: FirmwareSegment[]
    /** 被忽略的 RAM-only PT_LOAD 段数量（仅 ELF 可能非 0） */
    ignoredRamSegments: number
    /** 镜像中未被任何段覆盖、以 0xFF 填充的字节数 */
    filledGapBytes: number
    warnings: string[]
}

interface DataChunk {
    address: number
    data: Uint8Array
}

// ---------------------------------------------------------------------------
// 基础读取
// ---------------------------------------------------------------------------
function readU16(bytes: Uint8Array, offset: number): number {
    if (offset < 0 || offset + 2 > bytes.length) throw new FirmwareImageError('ELF: truncated program header')
    return bytes[offset] | (bytes[offset + 1] << 8)
}

function readU32(bytes: Uint8Array, offset: number): number {
    if (offset < 0 || offset + 4 > bytes.length) throw new FirmwareImageError('ELF: truncated program header')
    return (
        (bytes[offset] |
            (bytes[offset + 1] << 8) |
            (bytes[offset + 2] << 16) |
            (bytes[offset + 3] << 24)) >>>
        0
    )
}

/** 二进制 -> ASCII 文本（HEX/SREC 都是纯 ASCII；逐块构造避免超大参数）。 */
function decodeAscii(bytes: Uint8Array): string {
    let text = ''
    const chunk = 8192
    for (let offset = 0; offset < bytes.length; offset += chunk) {
        text += String.fromCharCode(...bytes.subarray(offset, Math.min(offset + chunk, bytes.length)))
    }
    return text
}

/** 解析偶数长度 hex 字符串；失败抛错。 */
function parseHexBytes(text: string, what: string): Uint8Array {
    if (text.length % 2 !== 0) throw new FirmwareImageError(`${what}: odd hex string length`)
    if (!/^[0-9a-fA-F]*$/.test(text)) throw new FirmwareImageError(`${what}: invalid hex characters`)
    const out = new Uint8Array(text.length / 2)
    for (let i = 0; i < out.length; i++) out[i] = Number.parseInt(text.slice(i * 2, i * 2 + 2), 16)
    return out
}

// ---------------------------------------------------------------------------
// 镜像组装（统一校验/填充）
// ---------------------------------------------------------------------------
/** 合并连续数据块：HEX/SREC 通常每条记录一块，展示/填充前先合并成连续段。 */
function mergeChunks(chunks: readonly DataChunk[]): DataChunk[] {
    const sorted = chunks.filter((chunk) => chunk.data.length > 0).slice().sort((a, b) => a.address - b.address)
    const merged: Array<{address: number; parts: Uint8Array[]; length: number}> = []
    for (const chunk of sorted) {
        if (chunk.address + chunk.data.length > 0xffffffff) throw new FirmwareImageError('data address overflow')
        const last = merged[merged.length - 1]
        if (last && chunk.address <= last.address + last.length) {
            const coveredEnd = last.address + last.length
            if (chunk.address + chunk.data.length > coveredEnd) {
                // 只追加超出当前覆盖范围的尾部（重叠部分保留先写入的数据）
                last.parts.push(chunk.data.subarray(coveredEnd - chunk.address))
                last.length = chunk.address + chunk.data.length - last.address
            }
            continue
        }
        merged.push({address: chunk.address, parts: [chunk.data], length: chunk.data.length})
    }
    return merged.map((range) => {
        if (range.parts.length === 1) return {address: range.address, data: range.parts[0]}
        const data = new Uint8Array(range.length)
        let offset = 0
        for (const part of range.parts) {
            data.set(part, offset)
            offset += part.length
        }
        return {address: range.address, data}
    })
}

/** 小空隙（段间对齐 padding）阈值：HEX/SREC 常省略这些 0x00 对齐字节。 */
const SMALL_GAP_THRESHOLD = 64

function buildImage(
    format: FirmwareFormat,
    flashBase: number,
    flashSize: number,
    rawChunks: readonly DataChunk[],
    sourceSize: number,
    ignoredRamSegments = 0,
): FirmwareImage {
    const chunks = mergeChunks(rawChunks)
    if (chunks.length === 0) throw new FirmwareImageError('no flash data found in firmware file')
    const minAddress = chunks[0].address
    let maxAddress = 0
    for (const chunk of chunks) {
        const end = chunk.address + chunk.data.length
        if (end > maxAddress) maxAddress = end
    }
    if (minAddress < flashBase) {
        throw new FirmwareImageError(
            `data address 0x${minAddress.toString(16)} is below flash base 0x${flashBase.toString(16)}`,
        )
    }
    if (maxAddress > flashBase + flashSize) {
        throw new FirmwareImageError(
            `data ends at 0x${maxAddress.toString(16)}, beyond flash end 0x${(flashBase + flashSize).toString(16)}`,
        )
    }
    const length = maxAddress - flashBase
    if (length <= 0) throw new FirmwareImageError('empty firmware image')
    if (length > flashSize) throw new FirmwareImageError('firmware image exceeds flash size')

    const image = new Uint8Array(length)
    image.fill(0xff)
    const covered = new Uint8Array(length)
    const segments: FirmwareSegment[] = []
    let coveredBytes = 0
    for (const chunk of chunks) {
        const offset = chunk.address - flashBase
        image.set(chunk.data, offset)
        for (let i = 0; i < chunk.data.length; i++) {
            if (covered[offset + i] === 0) {
                covered[offset + i] = 1
                coveredBytes += 1
            }
        }
        segments.push({address: chunk.address, size: chunk.data.length})
    }

    // 空隙填充：首尾/大空隙填 0xFF（未编程区）；HEX/SREC 的段间小空隙是
    // 链接对齐 padding，源 .bin 里通常是 0x00，填 0x00 才能与官方 .bin 一致。
    const warnings: string[] = []
    let filledZeroBytes = 0
    let filledEraseBytes = 0
    for (let offset = 0; offset < length; ) {
        if (covered[offset] !== 0) {
            offset += 1
            continue
        }
        let end = offset
        while (end < length && covered[end] === 0) end += 1
        const isInternal = offset > 0 && end < length
        const fillZero = isInternal && end - offset <= SMALL_GAP_THRESHOLD
        if (fillZero) {
            image.fill(0x00, offset, end)
            filledZeroBytes += end - offset
        } else {
            filledEraseBytes += end - offset
        }
        offset = end
    }
    if (filledZeroBytes > 0) warnings.push(`filled ${filledZeroBytes} alignment gap byte(s) with 0x00`)
    if (filledEraseBytes > 0) warnings.push(`filled ${filledEraseBytes} gap byte(s) with 0xFF`)
    return {
        format,
        image,
        base: flashBase,
        sourceSize,
        segments,
        ignoredRamSegments,
        filledGapBytes: filledZeroBytes + filledEraseBytes,
        warnings,
    }
}

// ---------------------------------------------------------------------------
// 格式检测
// ---------------------------------------------------------------------------
const EXTENSION_FORMATS: Record<string, FirmwareFormat> = {
    bin: 'bin',
    elf: 'elf',
    out: 'elf',
    axf: 'elf',
    hex: 'hex',
    ihex: 'hex',
    srec: 'srec',
    s19: 'srec',
    s28: 'srec',
    s37: 'srec',
    mot: 'srec',
}

/**
 * 按内容魔数优先、扩展名兜底识别固件格式；无法识别返回 null。
 * 内容：ELF `7F 45 4C 46`、Intel HEX `:` 开头、S-Record `S<digit>` 开头。
 */
export function detectFirmwareFormat(fileName: string, bytes: Uint8Array | null): FirmwareFormat | null {
    if (bytes && bytes.length >= 4) {
        if (bytes[0] === 0x7f && bytes[1] === 0x45 && bytes[2] === 0x4c && bytes[3] === 0x46) return 'elf'
        const head = decodeAscii(bytes.subarray(0, Math.min(bytes.length, 16))).replace(/^\s+/, '')
        if (head.startsWith(':')) return 'hex'
        if (/^S[0-9]/.test(head)) return 'srec'
    }
    const match = /\.([a-z0-9]+)$/i.exec(fileName)
    if (!match) return null
    return EXTENSION_FORMATS[match[1].toLowerCase()] ?? null
}

// ---------------------------------------------------------------------------
// ELF32 LE ARM
// ---------------------------------------------------------------------------
const PT_LOAD = 1
const EM_ARM = 40
const ELF_HEADER_SIZE = 52
const ELF_PHDR_SIZE = 32

export function parseElfImage(bytes: Uint8Array, flashBase: number, flashSize: number): FirmwareImage {
    if (bytes.length < ELF_HEADER_SIZE) throw new FirmwareImageError('ELF: file is too small')
    if (!(bytes[0] === 0x7f && bytes[1] === 0x45 && bytes[2] === 0x4c && bytes[3] === 0x46)) {
        throw new FirmwareImageError('ELF: bad magic')
    }
    if (bytes[4] !== 1) throw new FirmwareImageError('ELF: only ELF32 is supported')
    if (bytes[5] !== 1) throw new FirmwareImageError('ELF: only little-endian is supported')
    const machine = readU16(bytes, 18)
    if (machine !== EM_ARM) throw new FirmwareImageError(`ELF: unsupported machine ${machine} (ARM only)`)
    const phoff = readU32(bytes, 28)
    const phentsize = readU16(bytes, 42)
    const phnum = readU16(bytes, 44)
    if (phnum <= 0) throw new FirmwareImageError('ELF: no program headers')
    if (phentsize < ELF_PHDR_SIZE) throw new FirmwareImageError('ELF: bad program header size')
    if (phoff + phnum * phentsize > bytes.length) throw new FirmwareImageError('ELF: program headers out of range')

    const flashEnd = flashBase + flashSize
    const chunks: DataChunk[] = []
    let ignoredRamSegments = 0
    for (let index = 0; index < phnum; index++) {
        const at = phoff + index * phentsize
        const type = readU32(bytes, at)
        if (type !== PT_LOAD) continue
        const offset = readU32(bytes, at + 4)
        const vaddr = readU32(bytes, at + 8)
        const paddr = readU32(bytes, at + 12)
        const fileSize = readU32(bytes, at + 16)
        // 关键：烧录使用 LMA。RW 段的 p_paddr 通常是 Flash 初始化副本地址；
        // 仅当 p_paddr 为 0 时才回退 p_vaddr。
        const lma = paddr !== 0 ? paddr : vaddr
        if (fileSize === 0) {
            // .bss / heap / stack 等 NOBITS 段：无数据可烧录，计入忽略的 RAM 段。
            if (lma < flashBase || lma >= flashEnd) ignoredRamSegments += 1
            continue
        }
        if (offset + fileSize > bytes.length) throw new FirmwareImageError('ELF: segment data out of range (truncated file)')
        if (lma >= flashBase && lma + fileSize <= flashEnd) {
            chunks.push({address: lma, data: bytes.subarray(offset, offset + fileSize)})
        } else if (lma + fileSize <= flashBase || lma >= flashEnd) {
            ignoredRamSegments += 1
        } else {
            throw new FirmwareImageError(
                `ELF: segment 0x${lma.toString(16)}+0x${fileSize.toString(16)} crosses the flash range boundary`,
            )
        }
    }
    if (chunks.length === 0) throw new FirmwareImageError('ELF: no loadable segment inside the target flash')
    return buildImage('elf', flashBase, flashSize, chunks, bytes.length, ignoredRamSegments)
}

// ---------------------------------------------------------------------------
// Intel HEX
// ---------------------------------------------------------------------------
export function parseIntelHex(text: string, flashBase: number, flashSize: number): FirmwareImage {
    const lines = text.split(/\r?\n/)
    const chunks: DataChunk[] = []
    let extendedBase = 0
    let sawEof = false
    for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
        const line = lines[lineIndex].trim()
        if (!line) continue
        if (!line.startsWith(':')) {
            throw new FirmwareImageError(`Intel HEX: invalid record at line ${lineIndex + 1}`)
        }
        const record = parseHexBytes(line.slice(1), `Intel HEX line ${lineIndex + 1}`)
        if (record.length < 5) throw new FirmwareImageError(`Intel HEX: record too short at line ${lineIndex + 1}`)
        const byteCount = record[0]
        if (record.length !== byteCount + 5) {
            throw new FirmwareImageError(`Intel HEX: length mismatch at line ${lineIndex + 1}`)
        }
        let sum = 0
        for (const byte of record) sum = (sum + byte) & 0xff
        if (sum !== 0) throw new FirmwareImageError(`Intel HEX: checksum error at line ${lineIndex + 1}`)
        const address = (record[1] << 8) | record[2]
        const recordType = record[3]
        const data = record.subarray(4, 4 + byteCount)
        switch (recordType) {
            case 0x00: {
                chunks.push({address: (extendedBase + address) >>> 0, data})
                break
            }
            case 0x01: {
                sawEof = true
                break
            }
            case 0x02: {
                if (byteCount !== 2) throw new FirmwareImageError('Intel HEX: bad extended segment address record')
                extendedBase = (((data[0] << 8) | data[1]) << 4) >>> 0
                break
            }
            case 0x03: {
                // Start Segment Address：与烧录镜像无关
                break
            }
            case 0x04: {
                if (byteCount !== 2) throw new FirmwareImageError('Intel HEX: bad extended linear address record')
                extendedBase = (((data[0] << 8) | data[1]) << 16) >>> 0
                break
            }
            case 0x05: {
                // Start Linear Address：与烧录镜像无关
                break
            }
            default:
                throw new FirmwareImageError(`Intel HEX: unsupported record type 0x${recordType.toString(16)}`)
        }
        if (sawEof) break
    }
    if (chunks.length === 0) throw new FirmwareImageError('Intel HEX: no data records')
    return buildImage('hex', flashBase, flashSize, chunks, text.length)
}

// ---------------------------------------------------------------------------
// Motorola S-Record
// ---------------------------------------------------------------------------
export function parseSrec(text: string, flashBase: number, flashSize: number): FirmwareImage {
    const lines = text.split(/\r?\n/)
    const chunks: DataChunk[] = []
    for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
        const line = lines[lineIndex].trim()
        if (!line) continue
        if (!/^S[0-9]/.test(line)) {
            throw new FirmwareImageError(`SREC: invalid record at line ${lineIndex + 1}`)
        }
        const recordType = line[1]
        const record = parseHexBytes(line.slice(2), `SREC line ${lineIndex + 1}`)
        if (record.length < 2) throw new FirmwareImageError(`SREC: record too short at line ${lineIndex + 1}`)
        const count = record[0]
        if (record.length !== count + 1) {
            throw new FirmwareImageError(`SREC: length mismatch at line ${lineIndex + 1}`)
        }
        let sum = 0
        for (const byte of record) sum = (sum + byte) & 0xff
        if (sum !== 0xff) throw new FirmwareImageError(`SREC: checksum error at line ${lineIndex + 1}`)
        switch (recordType) {
            case '0': // 文件头（工具/文件名），忽略
            case '5': // 16-bit 记录计数
            case '6': // 24-bit 记录计数
                break
            case '1':
            case '2':
            case '3': {
                const addressLength = recordType === '1' ? 2 : recordType === '2' ? 3 : 4
                if (record.length < 1 + addressLength + 1) {
                    throw new FirmwareImageError(`SREC: data record too short at line ${lineIndex + 1}`)
                }
                let address = 0
                for (let i = 0; i < addressLength; i++) {
                    address = ((address << 8) | record[1 + i]) >>> 0
                }
                const data = record.subarray(1 + addressLength, record.length - 1)
                chunks.push({address, data})
                break
            }
            case '7': // 32-bit 启动地址
            case '8': // 24-bit 启动地址
            case '9': // 16-bit 启动地址
                break
            default:
                throw new FirmwareImageError(`SREC: unsupported record type S${recordType}`)
        }
    }
    if (chunks.length === 0) throw new FirmwareImageError('SREC: no data records')
    return buildImage('srec', flashBase, flashSize, chunks, text.length)
}

// ---------------------------------------------------------------------------
// 文件入口
// ---------------------------------------------------------------------------
/**
 * 读取并转换固件文件；flashBase/flashSize 为目标芯片参数
 * （`GET /api/target/info` 的 flash_base / flash_size）。
 * @throws {FirmwareImageError}
 */
export async function firmwareImageFromFile(
    file: File,
    flashBase: number,
    flashSize: number,
): Promise<FirmwareImage> {
    if (flashBase < 0 || flashSize <= 0) throw new FirmwareImageError('invalid target flash range')
    const bytes = new Uint8Array(await file.arrayBuffer())
    if (bytes.length === 0) throw new FirmwareImageError('file is empty')
    const format = detectFirmwareFormat(file.name, bytes)
    if (format === null) {
        throw new FirmwareImageError(`unsupported firmware format: ${file.name}`)
    }
    switch (format) {
        case 'bin': {
            if (bytes.length > flashSize) throw new FirmwareImageError('BIN file is larger than the target flash')
            return {
                format,
                image: bytes,
                base: flashBase,
                sourceSize: bytes.length,
                segments: [{address: flashBase, size: bytes.length}],
                ignoredRamSegments: 0,
                filledGapBytes: 0,
                warnings: [],
            }
        }
        case 'elf':
            return parseElfImage(bytes, flashBase, flashSize)
        case 'hex':
            return parseIntelHex(decodeAscii(bytes), flashBase, flashSize)
        case 'srec':
            return parseSrec(decodeAscii(bytes), flashBase, flashSize)
        default:
            throw new FirmwareImageError('unsupported firmware format')
    }
}
