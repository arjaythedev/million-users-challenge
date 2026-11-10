/// gzip in F#: greedy LZ77 (one hash probe per position, 32 KB window) and one DEFLATE block with Huffman codes built
/// for that body (lengths limited to 15 bits), or the fixed codes when those come out smaller; CRC-32 by slicing-by-8.
/// One Encoder per I/O thread (it keeps its hash table and scratch arrays between calls; nothing about a body survives
/// a call: positions from earlier calls are told apart by a running base, not by clearing the table).
module Deflate

#nowarn "9"

open System
open System.Buffers.Binary
open System.Numerics
open System.Runtime.CompilerServices
open Microsoft.FSharp.NativeInterop

// ---------- CRC-32 (IEEE 802.3, reflected), slicing-by-8 ----------

let private crcT =
    let t = Array.zeroCreate<uint32> (8 * 256)
    for i in 0 .. 255 do
        let mutable c = uint32 i
        for _ in 0 .. 7 do
            c <- if c &&& 1u <> 0u then 0xEDB88320u ^^^ (c >>> 1) else c >>> 1
        t.[i] <- c
    for k in 1 .. 7 do
        for i in 0 .. 255 do
            let p = t.[(k - 1) * 256 + i]
            t.[k * 256 + i] <- (p >>> 8) ^^^ t.[int (p &&& 0xFFu)]
    t

let crc32 (data: ReadOnlySpan<byte>) =
    let t = crcT
    let mutable c = 0xFFFFFFFFu
    let mutable d = data
    while d.Length >= 8 do
        let a = c ^^^ BinaryPrimitives.ReadUInt32LittleEndian d
        let b = BinaryPrimitives.ReadUInt32LittleEndian(d.Slice 4)
        c <-
            t.[1792 + int (a &&& 0xFFu)] ^^^ t.[1536 + int ((a >>> 8) &&& 0xFFu)]
            ^^^ t.[1280 + int ((a >>> 16) &&& 0xFFu)] ^^^ t.[1024 + int (a >>> 24)]
            ^^^ t.[768 + int (b &&& 0xFFu)] ^^^ t.[512 + int ((b >>> 8) &&& 0xFFu)]
            ^^^ t.[256 + int ((b >>> 16) &&& 0xFFu)] ^^^ t.[int (b >>> 24)]
        d <- d.Slice 8
    for x in d do
        c <- t.[int ((c ^^^ uint32 x) &&& 0xFFu)] ^^^ (c >>> 8)
    ~~~c

// ---------- DEFLATE tables (RFC 1951) ----------

let private lenBase = [| 3; 4; 5; 6; 7; 8; 9; 10; 11; 13; 15; 17; 19; 23; 27; 31; 35; 43; 51; 59; 67; 83; 99; 115; 131; 163; 195; 227; 258 |]
let private lenExtra = [| 0; 0; 0; 0; 0; 0; 0; 0; 1; 1; 1; 1; 2; 2; 2; 2; 3; 3; 3; 3; 4; 4; 4; 4; 5; 5; 5; 5; 0 |]
let private distBase =
    [| 1; 2; 3; 4; 5; 7; 9; 13; 17; 25; 33; 49; 65; 97; 129; 193; 257; 385; 513; 769; 1025; 1537; 2049; 3073; 4097; 6145; 8193; 12289; 16385; 24577 |]
let private distExtra = [| 0; 0; 0; 0; 1; 1; 2; 2; 3; 3; 4; 4; 5; 5; 6; 6; 7; 7; 8; 8; 9; 9; 10; 10; 11; 11; 12; 12; 13; 13 |]
/// length (3..258) -> length code index 0..28
let private lenCode =
    let t = Array.zeroCreate<byte> 259
    for c in 0 .. 28 do
        let hi = if c = 28 then 258 else lenBase.[c] + (1 <<< lenExtra.[c]) - 1
        for l in lenBase.[c] .. min 258 hi do t.[l] <- byte c
    t.[258] <- 28uy
    t
/// distance - 1: < 256 direct, else 256 + ((d - 1) >>> 7)
let private distCode =
    let t = Array.zeroCreate<byte> 512
    for c in 0 .. 29 do
        let lo = distBase.[c]
        let hi = lo + (1 <<< distExtra.[c]) - 1
        for d in lo .. hi do
            if d - 1 < 256 then t.[d - 1] <- byte c
            else t.[256 + ((d - 1) >>> 7)] <- byte c
    t
let inline private dcode (dist: int) = if dist <= 256 then int distCode.[dist - 1] else int distCode.[256 + ((dist - 1) >>> 7)]
let private clOrder = [| 16; 17; 18; 0; 8; 7; 9; 6; 10; 5; 11; 4; 12; 3; 13; 2; 14; 1; 15 |]

let private rev8 =
    Array.init 256 (fun i ->
        let mutable r = 0
        for b in 0 .. 7 do
            if i &&& (1 <<< b) <> 0 then r <- r ||| (1 <<< (7 - b))
        byte r)
let inline private reverse (code: int) (len: int) =
    ((int rev8.[code &&& 0xFF] <<< 8) ||| int rev8.[(code >>> 8) &&& 0xFF]) >>> (16 - len)

/// canonical codes (bit-reversed for LSB-first output) from code lengths
let private canonical (lens: byte[]) (n: int) (codes: uint16[]) (blCount: int[]) (nextCode: int[]) =
    Array.Clear(blCount)
    for i in 0 .. n - 1 do
        if lens.[i] > 0uy then blCount.[int lens.[i]] <- blCount.[int lens.[i]] + 1
    blCount.[0] <- 0
    let mutable code = 0
    for b in 1 .. 15 do
        code <- (code + blCount.[b - 1]) <<< 1
        nextCode.[b] <- code
    for i in 0 .. n - 1 do
        let l = int lens.[i]
        if l > 0 then
            codes.[i] <- uint16 (reverse nextCode.[l] l)
            nextCode.[l] <- nextCode.[l] + 1

let private fixedLit =
    let l = Array.zeroCreate<byte> 288
    for i in 0 .. 143 do l.[i] <- 8uy
    for i in 144 .. 255 do l.[i] <- 9uy
    for i in 256 .. 279 do l.[i] <- 7uy
    for i in 280 .. 287 do l.[i] <- 8uy
    l
let private fixedDist = Array.create 30 5uy
let private fixedLitCodes =
    let c = Array.zeroCreate<uint16> 288
    canonical fixedLit 288 c (Array.zeroCreate 16) (Array.zeroCreate 16)
    c
let private fixedDistCodes =
    let c = Array.zeroCreate<uint16> 30
    canonical fixedDist 30 c (Array.zeroCreate 16) (Array.zeroCreate 16)
    c

[<Literal>]
let private HBits = 13
[<Literal>]
let private Window = 32768

/// One per thread. `Gzip` writes a gzip member for `input` into `output` and returns its length (0: didn't fit).
[<Sealed>]
type Encoder() =
    let head = Array.zeroCreate<int> (1 <<< HBits)
    let mutable baseP = 1 // positions are stored as pos + baseP; anything below baseP is from an earlier call
    let mutable tokens = Array.zeroCreate<int> 4096
    let litFreq = Array.zeroCreate<int> 286
    let distFreq = Array.zeroCreate<int> 30
    let litLen = Array.zeroCreate<byte> 286
    let distLen = Array.zeroCreate<byte> 30
    let litCodes = Array.zeroCreate<uint16> 286
    let distCodes = Array.zeroCreate<uint16> 30
    let clFreq = Array.zeroCreate<int> 19
    let clLen = Array.zeroCreate<byte> 19
    let clCodes = Array.zeroCreate<uint16> 19
    let rle = Array.zeroCreate<int> (286 + 30) // symbol | extra << 8
    let lens = Array.zeroCreate<byte> (286 + 30)
    let blCount = Array.zeroCreate<int> 16
    let nextCode = Array.zeroCreate<int> 16
    // Huffman scratch
    let sym = Array.zeroCreate<int> 286
    let work = Array.zeroCreate<int> 286
    let depthCount = Array.zeroCreate<int> 300
    // bit writer
    let mutable bits = 0UL
    let mutable nbits = 0
    let mutable outPos = 0

    /// Moffat-Katajainen code lengths for symbols with freq > 0, limited to `limit` bits (JPEG Annex K.3 adjust)
    member private _.Lengths(freq: int[], n: int, lensOut: byte[], limit: int) =
        Array.Clear(lensOut, 0, n)
        let mutable m = 0
        for i in 0 .. n - 1 do
            if freq.[i] > 0 then
                sym.[m] <- i
                m <- m + 1
        if m = 0 then
            lensOut.[0] <- 1uy
            lensOut.[1] <- 1uy
        elif m = 1 then
            lensOut.[sym.[0]] <- 1uy
            lensOut.[(if sym.[0] = 0 then 1 else 0)] <- 1uy
        else
            // sort by frequency ascending, ties by symbol: keys freq << 9 | symbol (freq < 2^22 for bodies < 4 MB)
            let syms = Span<int>(sym, 0, m)
            for i in 0 .. m - 1 do syms.[i] <- (freq.[syms.[i]] <<< 9) ||| syms.[i]
            syms.Sort()
            for i in 0 .. m - 1 do syms.[i] <- syms.[i] &&& 0x1FF
            let a = work
            for i in 0 .. m - 1 do a.[i] <- freq.[syms.[i]]
            // phase 1: build the tree in place
            a.[0] <- a.[0] + a.[1]
            let mutable root = 0
            let mutable leaf = 2
            for next in 1 .. m - 2 do
                if leaf >= m || a.[root] < a.[leaf] then
                    a.[next] <- a.[root]
                    a.[root] <- next
                    root <- root + 1
                else
                    a.[next] <- a.[leaf]
                    leaf <- leaf + 1
                if leaf >= m || (root < next && a.[root] < a.[leaf]) then
                    a.[next] <- a.[next] + a.[root]
                    a.[root] <- next
                    root <- root + 1
                else
                    a.[next] <- a.[next] + a.[leaf]
                    leaf <- leaf + 1
            // phase 2: internal node depths
            a.[m - 2] <- 0
            for next in m - 3 .. -1 .. 0 do a.[next] <- a.[a.[next]] + 1
            // phase 3: leaf depths
            let mutable avbl = 1
            let mutable used = 0
            let mutable dpth = 0
            let mutable root2 = m - 2
            let mutable next = m - 1
            while avbl > 0 do
                while root2 >= 0 && a.[root2] = dpth do
                    used <- used + 1
                    root2 <- root2 - 1
                while avbl > used do
                    a.[next] <- dpth
                    next <- next - 1
                    avbl <- avbl - 1
                avbl <- 2 * used
                dpth <- dpth + 1
                used <- 0
            // a[i] = length of syms[i] (ascending frequency -> non-increasing length). Limit the lengths.
            let mutable maxLen = 0
            Array.Clear(depthCount)
            for i in 0 .. m - 1 do
                depthCount.[a.[i]] <- depthCount.[a.[i]] + 1
                if a.[i] > maxLen then maxLen <- a.[i]
            if maxLen > limit then
                for i in maxLen .. -1 .. limit + 1 do
                    while depthCount.[i] > 0 do
                        let mutable j = i - 2
                        while depthCount.[j] = 0 do j <- j - 1
                        depthCount.[i] <- depthCount.[i] - 2
                        depthCount.[i - 1] <- depthCount.[i - 1] + 1
                        depthCount.[j + 1] <- depthCount.[j + 1] + 2
                        depthCount.[j] <- depthCount.[j] - 1
                // hand the lengths out again: the most frequent symbols get the shortest
                let mutable k = m - 1
                for l in 1 .. limit do
                    for _ in 1 .. depthCount.[l] do
                        a.[k] <- l
                        k <- k - 1
            for i in 0 .. m - 1 do lensOut.[syms.[i]] <- byte a.[i]

    member inline private _.Put(output: Span<byte>, value: uint64, n: int) =
        bits <- bits ||| (value <<< nbits)
        nbits <- nbits + n
        if nbits >= 32 then
            if outPos + 4 <= output.Length then BinaryPrimitives.WriteUInt32LittleEndian(output.Slice outPos, uint32 bits)
            outPos <- outPos + 4
            bits <- bits >>> 32
            nbits <- nbits - 32

    member private this.Flush(output: Span<byte>) =
        while nbits > 0 do
            if outPos < output.Length then output.[outPos] <- byte bits
            outPos <- outPos + 1
            bits <- bits >>> 8
            nbits <- nbits - 8
        nbits <- 0
        bits <- 0UL

    /// LZ77 into tokens; fills litFreq/distFreq. Token: literal byte, or 0x80000000 | dist << 9 | len.
    member private _.Parse(p: nativeptr<byte>, n: int) : int =
        if tokens.Length < n + 1 then tokens <- Array.zeroCreate (max (n + 1) (tokens.Length * 2))
        if baseP > Int32.MaxValue - 2 * n - 2 * Window then
            Array.Clear(head)
            baseP <- 1
        let b = baseP
        Array.Clear(litFreq)
        Array.Clear(distFreq)
        let tok = tokens
        let hd = head
        let lf = litFreq
        let df = distFreq
        let lcode = lenCode
        let inline rd32 (i: int) = NativePtr.read (NativePtr.ofNativeInt<uint32> (NativePtr.toNativeInt p + nativeint i))
        let inline rd64 (i: int) = NativePtr.read (NativePtr.ofNativeInt<uint64> (NativePtr.toNativeInt p + nativeint i))
        let mutable nt = 0
        let mutable i = 0
        let shift = 32 - HBits
        let last4 = n - 4
        while i < n do
            let mutable step = 0
            if i <= last4 then
                let v = rd32 i
                let h = int ((v * 2654435761u) >>> shift)
                let cand = hd.[h] - b
                hd.[h] <- i + b
                if cand >= 0 && i - cand <= Window && rd32 cand = v then
                    let maxL = min 258 (n - i)
                    let mutable l = 4
                    let mutable go = true
                    while go && l + 8 <= maxL do
                        let x = rd64 (cand + l) ^^^ rd64 (i + l)
                        if x = 0UL then l <- l + 8
                        else
                            l <- l + (BitOperations.TrailingZeroCount x >>> 3)
                            go <- false
                    if go then
                        while l < maxL && NativePtr.get p (cand + l) = NativePtr.get p (i + l) do l <- l + 1
                    let dist = i - cand
                    tok.[nt] <- 0x80000000 ||| (dist <<< 9) ||| l
                    nt <- nt + 1
                    let lc = 257 + int lcode.[l]
                    lf.[lc] <- lf.[lc] + 1
                    let dc = dcode dist
                    df.[dc] <- df.[dc] + 1
                    // index the positions inside the match: all of a short one, the last three of a long one
                    let stop = min (i + l) (last4 + 1)
                    let mutable j = if l <= 16 then i + 1 else i + l - 3
                    while j < stop do
                        hd.[int ((rd32 j * 2654435761u) >>> shift)] <- j + b
                        j <- j + 1
                    step <- l
            if step = 0 then
                let c = int (NativePtr.get p i)
                tok.[nt] <- c
                nt <- nt + 1
                lf.[c] <- lf.[c] + 1
                step <- 1
            i <- i + step
        baseP <- baseP + n + Window + 1
        lf.[256] <- lf.[256] + 1
        nt

    member private this.WriteTokens(output: Span<byte>, nt: int, lc: uint16[], ll: byte[], dcs: uint16[], dl: byte[]) =
        let mutable bb = bits
        let mutable nb = nbits
        let mutable pos = outPos
        let cap = output.Length - 8
        let tok = tokens
        use po = fixed &System.Runtime.InteropServices.MemoryMarshal.GetReference output
        let pb = NativePtr.toNativeInt po
        for t in 0 .. nt + 1 - 1 do
            let x = if t = nt then 256 else tok.[t]
            if x >= 0 then
                bb <- bb ||| (uint64 lc.[x] <<< nb)
                nb <- nb + int ll.[x]
            else
                let l = x &&& 0x1FF
                let dist = (x >>> 9) &&& 0x3FFFFF
                let c = int lenCode.[l]
                bb <- bb ||| (uint64 lc.[257 + c] <<< nb)
                nb <- nb + int ll.[257 + c]
                bb <- bb ||| (uint64 (l - lenBase.[c]) <<< nb)
                nb <- nb + lenExtra.[c]
                if nb >= 32 then
                    if pos <= cap then NativePtr.write (NativePtr.ofNativeInt<uint32> (pb + nativeint pos)) (uint32 bb)
                    pos <- pos + 4
                    bb <- bb >>> 32
                    nb <- nb - 32
                let d = dcode dist
                bb <- bb ||| (uint64 dcs.[d] <<< nb)
                nb <- nb + int dl.[d]
                bb <- bb ||| (uint64 (dist - distBase.[d]) <<< nb)
                nb <- nb + distExtra.[d]
            if nb >= 32 then
                if pos <= cap then NativePtr.write (NativePtr.ofNativeInt<uint32> (pb + nativeint pos)) (uint32 bb)
                pos <- pos + 4
                bb <- bb >>> 32
                nb <- nb - 32
        bits <- bb
        nbits <- nb
        outPos <- pos

    /// gzip member of `input` into `output`; returns its length, or 0 if `output` is too small
    member this.Gzip(input: ReadOnlySpan<byte>, output: Span<byte>) : int =
        let nt =
            use pi = fixed &System.Runtime.InteropServices.MemoryMarshal.GetReference input
            this.Parse(pi, input.Length)
        // dynamic code lengths
        this.Lengths(litFreq, 286, litLen, 15)
        this.Lengths(distFreq, 30, distLen, 15)
        let mutable hlit = 286
        while hlit > 257 && litLen.[hlit - 1] = 0uy do hlit <- hlit - 1
        let mutable hdist = 30
        while hdist > 1 && distLen.[hdist - 1] = 0uy do hdist <- hdist - 1
        // run-length encode the code lengths
        Array.Copy(litLen, 0, lens, 0, hlit)
        Array.Copy(distLen, 0, lens, hlit, hdist)
        let total = hlit + hdist
        Array.Clear(clFreq)
        let mutable nr = 0
        let mutable i = 0
        while i < total do
            let v = lens.[i]
            let mutable run = 1
            while i + run < total && lens.[i + run] = v do run <- run + 1
            let mutable r = run
            if v = 0uy then
                while r >= 11 do
                    let k = min r 138
                    rle.[nr] <- 18 ||| ((k - 11) <<< 8)
                    nr <- nr + 1
                    clFreq.[18] <- clFreq.[18] + 1
                    r <- r - k
                if r >= 3 then
                    rle.[nr] <- 17 ||| ((r - 3) <<< 8)
                    nr <- nr + 1
                    clFreq.[17] <- clFreq.[17] + 1
                    r <- 0
            else
                rle.[nr] <- int v
                nr <- nr + 1
                clFreq.[int v] <- clFreq.[int v] + 1
                r <- r - 1
                while r >= 3 do
                    let k = min r 6
                    rle.[nr] <- 16 ||| ((k - 3) <<< 8)
                    nr <- nr + 1
                    clFreq.[16] <- clFreq.[16] + 1
                    r <- r - k
            while r > 0 do
                rle.[nr] <- int v
                nr <- nr + 1
                clFreq.[int v] <- clFreq.[int v] + 1
                r <- r - 1
            i <- i + run
        this.Lengths(clFreq, 19, clLen, 7)
        let mutable hclen = 19
        while hclen > 4 && clLen.[clOrder.[hclen - 1]] = 0uy do hclen <- hclen - 1
        // sizes in bits: dynamic vs fixed
        let mutable dynBits = 3L + 5L + 5L + 4L + 3L * int64 hclen
        for k in 0 .. nr - 1 do
            let s = rle.[k] &&& 0xFF
            dynBits <- dynBits + int64 clLen.[s] + (match s with | 16 -> 2L | 17 -> 3L | 18 -> 7L | _ -> 0L)
        let mutable fixBits = 3L
        for s in 0 .. 285 do
            let f = int64 litFreq.[s]
            if f > 0L then
                let extra = if s >= 257 then int64 lenExtra.[s - 257] else 0L
                dynBits <- dynBits + f * (int64 litLen.[s] + extra)
                fixBits <- fixBits + f * (int64 fixedLit.[s] + extra)
        for s in 0 .. 29 do
            let f = int64 distFreq.[s]
            if f > 0L then
                dynBits <- dynBits + f * (int64 distLen.[s] + int64 distExtra.[s])
                fixBits <- fixBits + f * (5L + int64 distExtra.[s])
        // gzip header: magic, CM=8, FLG=0, MTIME=0, XFL=0, OS=255
        if output.Length < 18 then 0
        else
            output.[0] <- 0x1fuy
            output.[1] <- 0x8buy
            output.[2] <- 8uy
            output.Slice(3, 6).Clear()
            output.[9] <- 255uy
            outPos <- 10
            bits <- 0UL
            nbits <- 0
            if fixBits <= dynBits then
                this.Put(output, 3UL, 3) // BFINAL=1, BTYPE=01
                this.WriteTokens(output, nt, fixedLitCodes, fixedLit, fixedDistCodes, fixedDist)
            else
                canonical litLen 286 litCodes blCount nextCode
                canonical distLen 30 distCodes blCount nextCode
                canonical clLen 19 clCodes blCount nextCode
                this.Put(output, 5UL, 3) // BFINAL=1, BTYPE=10
                this.Put(output, uint64 (hlit - 257), 5)
                this.Put(output, uint64 (hdist - 1), 5)
                this.Put(output, uint64 (hclen - 4), 4)
                for k in 0 .. hclen - 1 do this.Put(output, uint64 clLen.[clOrder.[k]], 3)
                for k in 0 .. nr - 1 do
                    let s = rle.[k] &&& 0xFF
                    this.Put(output, uint64 clCodes.[s], int clLen.[s])
                    match s with
                    | 16 -> this.Put(output, uint64 (rle.[k] >>> 8), 2)
                    | 17 -> this.Put(output, uint64 (rle.[k] >>> 8), 3)
                    | 18 -> this.Put(output, uint64 (rle.[k] >>> 8), 7)
                    | _ -> ()
                this.WriteTokens(output, nt, litCodes, litLen, distCodes, distLen)
            this.Flush output
            if outPos + 8 > output.Length then 0
            else
                BinaryPrimitives.WriteUInt32LittleEndian(output.Slice outPos, crc32 input)
                BinaryPrimitives.WriteUInt32LittleEndian(output.Slice(outPos + 4), uint32 input.Length)
                outPos + 8
