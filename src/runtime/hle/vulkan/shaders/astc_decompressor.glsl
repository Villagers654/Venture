// Compute shader to perform ASTC decoding.
//
// Usage:
// #include "AstcDecompressor.glsl"
//
// main() {
//   uvec4 astcBlock = ... // read an ASTC block
//   astcDecoderInitialize(astcBlock, blockSize);
//   uvec2 posInBlock = uvec2(0, 0);  // which texel we want to decode in the block
//   uvec4 texel = astcDecodeTexel(pos);
// }
//
////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Please refer for the ASTC spec for all the details:
// https://www.khronos.org/registry/OpenGL/extensions/KHR/KHR_texture_compression_astc_hdr.txt
//
//
// Quick reminder of an ASTC block layout
// --------------------------------------
//
// Each ASTC block is 128 bits. From top to bottom (from bit 127 to bit 0), we have:
//    1. weight data (24 - 96 bits). Starts at bit 127 and grows down. (So it needs to be reversed)
//    2. extra CEM data. 0 bits if only 1 partition OR if CEM selector value in bits [24:23] is 00,
//       otherwise 2, 5 or 8 bits for 2, 3, or 4 partitions respectively.
//    3. color component selector (CCS) - 2 bits if dual plane is active, otherwise 0 bits.
//    4. Color endpoint data - variable length
//    5. CEM                 4 bits if single partition, else 6 bits.
//    6. partition seed     10 bits (13-22) - only if more than 1 partition
//    7. partition count     2 bits (11-12)
//    8. block mode         11 bits ( 0-10)
//
// Optimization ideas
// ------------------
//
//   1. Use a uniform buffer instead of static arrays to load the tables in AstcLookupTables.glsl
//   2. Investigate using SSBO or sampled image instead of storage image for the input.
//   3. Make decodeTrit() / decodeQuint() return a pair of values, since we always need at least 2
//   4. Look into which queue we use to run the shader, some GPUs may have a separate compute queue.
//   5. Use a `shared` variable to share the block data and common block config, once we change the
//      local group size to match the block size.
//
// Missing features
// ----------------
//
//   1. Make sure we cover all the cases where we should return the error color? See section C.2.24
//      Illegal Encodings for the full list.
//   2. Add support for 3D slices.
//   3. HDR support? Probably not worth it.

#include "astc_lookup_tables.glsl"

const uvec4 kErrorColor = uvec4(65535, 0, 65535, 65535);  // beautiful magenta, as per the spec

// Global variables ////////////////////////////////////////////////////////////////////////////////

uvec4 astcBlock;       // Full ASTC block data
uvec2 blockSize;       // Size of the ASTC block
bool decodeError;      // True if there's an error in the block.
bool voidExtent;       // True if void-extent block (all pixels are the same color)
bool dualPlane;        // True for dual plane blocks (a block with 2 sets of weights)
uvec2 weightGridSize;  // Width and height of the weight grid. Always <= blockSize.
uint numWeights;       // Number of weights
uvec3 weightEncoding;  // Number of trits (x), quints (y) and bits (z) to encode the weights.
uint weightDataSize;   // Size of the weight data in bits
uint numPartitions;    // Number of partitions (1-4)
uint partitionSeed;    // Determines which partition pattern we use (10 bits)

////////////////////////////////////////////////////////////////////////////////////////////////////

// Returns the number of bits needed to encode `numVals` values using a given encoding.
// encoding: number of trits (x), quints (y) and bits (z) used for the encoding.
uint getEncodingSize(uint numVals, uvec3 encoding) {
    // See section C.2.22.
    uvec2 tqBits = (numVals * encoding.xy * uvec2(8, 7) + uvec2(4, 2)) / uvec2(5, 3);
    return numVals * encoding.z + (tqBits.x + tqBits.y);
}

// This function sets all the global variables above
void astcDecoderInitialize(uvec4 blockData, uvec2 blockSize_) {
    astcBlock = blockData;
    blockSize = blockSize_;
    decodeError = false;

    voidExtent = (astcBlock[3] & 0x1FF) == 0x1FC;
    if (voidExtent) {
        // 2D LDR void extent: HDR flag is forbidden, reserved bits are 11.
        decodeError = (astcBlock[3] & 0xe00u) != 0xc00u;
        return;
    }

    const uint bits01 = bitfieldExtract(astcBlock[3], 0, 2);
    const uint bits23 = bitfieldExtract(astcBlock[3], 2, 2);
    const uint bit4 = bitfieldExtract(astcBlock[3], 4, 1);
    const uint bits56 = bitfieldExtract(astcBlock[3], 5, 2);
    const uint bits78 = bitfieldExtract(astcBlock[3], 7, 2);

    uint r;
    uint h = bitfieldExtract(astcBlock[3], 9, 1);
    dualPlane = bool(bitfieldExtract(astcBlock[3], 10, 1));

    // Refer to "Table C.2.8 - 2D Block Mode Layout"
    if (bits01 == 0) {
        r = bits23 << 1 | bit4;
        switch (bits78) {
            case 0:
                weightGridSize = uvec2(12, bits56 + 2);
                break;
            case 1:
                weightGridSize = uvec2(bits56 + 2, 12);
                break;
            case 2:
                weightGridSize = uvec2(bits56 + 6, bitfieldExtract(astcBlock[3], 9, 2) + 6);
                dualPlane = false;
                h = 0;
                break;
            case 3:
                if (bits56 == 0) {
                    weightGridSize = uvec2(6, 10);
                } else if (bits56 == 1) {
                    weightGridSize = uvec2(10, 6);
                } else {
                    decodeError = true;
                    return;
                }
        }
    } else {
        r = bits01 << 1 | bit4;
        switch (bits23) {
            case 0:
                weightGridSize = uvec2(bits78 + 4, bits56 + 2);
                break;
            case 1:
                weightGridSize = uvec2(bits78 + 8, bits56 + 2);
                break;
            case 2:
                weightGridSize = uvec2(bits56 + 2, bits78 + 8);
                break;
            case 3:
                if (bits78 >> 1 == 0) {
                    weightGridSize = uvec2(bits56 + 2, (bits78 & 1) + 6);
                } else {
                    weightGridSize = uvec2((bits78 & 1) + 2, bits56 + 2);
                }
        }
    }

    if (any(greaterThan(weightGridSize, blockSize))) {
        decodeError = true;
        return;
    }

    // weigths
    weightEncoding = kWeightEncodings[h << 3 | r];
    numWeights = (weightGridSize.x * weightGridSize.y) << int(dualPlane);
    weightDataSize = getEncodingSize(numWeights, weightEncoding);
    if (weightDataSize < 24 || weightDataSize > 96 || numWeights > 64) {
        decodeError = true;
        return;
    }

    numPartitions = bitfieldExtract(astcBlock[3], 11, 2) + 1;
    if (numPartitions > 1) {
        partitionSeed = bitfieldExtract(astcBlock[3], 13, 10);
    }

    if (dualPlane && numPartitions == 4) {
        decodeError = true;
        return;
    }
}

// Extracts a range of bits from a uvec4, treating it as a single 128-bit field.
// offset: index of the first bit to extract (0-127).
// numBits: number of bits to extract. (0-32). If numBits is 0, this returns 0.
// Result is undefined if offset >= 128 or offset + numBits > 128
uint extractBits(uvec4 data, uint offset, uint numBits) {
    if (numBits == 0) return 0;

    const uint i = 3 - offset / 32;
    const uint j = 3 - (offset + numBits - 1) / 32;
    const uint start = offset & 31;
    if (i == j) {
        // All the bits to extract are located on the same component of the vector
        return bitfieldExtract(data[i], int(start), int(numBits));
    } else {
        uint numLowBits = 32 - start;
        uint lowBits = bitfieldExtract(data[i], int(start), int(numLowBits));
        uint highBits = bitfieldExtract(data[j], 0, int(numBits - numLowBits));
        return (highBits << numLowBits) | lowBits;
    }
}

// Returns the CEM, a number between 0 and 15 that determines how the endpoints are encoded.
// Also sets a couple of output parameters:
// - startOfExtraCem: bit position of the start of the extra CEM
// - totalEndpoints: number of endpoints in the block, for all partitions.
// - baseEndpointIndex: index of the first endpoint for this partition
// Refer to "Section C.2.11  Color Endpoint Mode" for decoding details
uint decodeCEM(uint partitionIndex, out uint startOfExtraCem, out uint totalEndpoints,
               out uint baseEndpointIndex) {
    if (numPartitions == 1) {
        startOfExtraCem = 128 - weightDataSize;
        const uint cem = bitfieldExtract(astcBlock[3], 13, 4);
        totalEndpoints = 2 * (cem >> 2) + 2;
        baseEndpointIndex = 0;
        return cem;
    } else {
        const uint cemSelector = bitfieldExtract(astcBlock[3], 23, 2);
        const uint baseCem = bitfieldExtract(astcBlock[3], 25, 4);

        if (cemSelector == 0) {
            // We're in the multi-partition, single CEM case
            startOfExtraCem = 128 - weightDataSize;
            const uint endpointsPerPartition = 2 * (baseCem >> 2) + 2;
            totalEndpoints = endpointsPerPartition * numPartitions;
            baseEndpointIndex = endpointsPerPartition * partitionIndex;
            return baseCem;
        } else {
            // Refer to "Figure C.4" for the details of the encoding here.

            // Size in bits of the extra CEM data, which is located right after the weight data.
            const uint sizeOfExtraCem = 3 * numPartitions - 4;
            startOfExtraCem = 128 - weightDataSize - sizeOfExtraCem;

            // Extract the extra CEM data
            const uint extraCem = extractBits(astcBlock, startOfExtraCem, sizeOfExtraCem);
            const uint fullCem = extraCem << 4 | baseCem;

            const uint mValue =
                bitfieldExtract(fullCem, int(2 * partitionIndex + numPartitions), 2);
            const uint cValues = bitfieldExtract(fullCem, 0, int(numPartitions));

            // TODO(gregschlom): investigate whether a couple of small lookup tables would be more
            // efficient here.
            totalEndpoints = 2 * (cemSelector * numPartitions + bitCount(cValues));
            baseEndpointIndex = 2 * (cemSelector * partitionIndex +
                                     bitCount(bitfieldExtract(cValues, 0, int(partitionIndex))));
            uint baseClass = cemSelector - 1 + bitfieldExtract(cValues, int(partitionIndex), 1);
            return baseClass << 2 | mValue;
        }
    }
}

// Decodes a single trit within a block of 5.
// offset: bit offset where the block of trits starts, within the 128 bits of data
// numBits: how many bits are used to encode the LSB (0-6)
// i: index of the trit within the block (0-4)
// See section "C.2.12  Integer Sequence Encoding"
uint decodeTrit(uvec4 data, uint offset, uint numBits, uint i) {
    const int inumBits = int(numBits);

    // In the largest encoding possible (1 trit + 6 bits), the block is 38 bits long (5 * 6 + 8).
    // Since this wouldn't fit in 32 bits, we extract the low bits for the trit index 0 separately,
    // this way we only need at most 4 * 6 + 8 = 32 bits, which fits perfectly.
    const uint block = extractBits(data, offset + numBits, 4 * numBits + 8);

    // Extract the 8 bits that encode the pack of 5 trits
    // TODO(gregschlom): Optimization idea: if numbits == 0, then packedTrits = block. Worth doing?
    const uint packedTrits = bitfieldExtract(block, 0, 2) |
                             bitfieldExtract(block, 1 * inumBits + 2, 2) << 2 |
                             bitfieldExtract(block, 2 * inumBits + 4, 1) << 4 |
                             bitfieldExtract(block, 3 * inumBits + 5, 2) << 5 |
                             bitfieldExtract(block, 4 * inumBits + 7, 1) << 7;

    // Extract the LSB
    uint lowBits;
    if (i == 0) {
        lowBits = extractBits(data, offset, numBits);
    } else {
        const int j = int(i) - 1;
        const ivec4 deltas = {2, 4, 5, 7};
        lowBits = bitfieldExtract(block, j * inumBits + deltas[j], inumBits);
    }

    const uint decoded = kTritEncodings[packedTrits];
    return bitfieldExtract(decoded, 2 * int(i), 2) << numBits | lowBits;
}

// Decodes a single quint within a block of 3.
// offset: bit offset where the block of quint starts, within the 128 bits of data
// numBits: how many bits are used to encode the LSB (0-5)
// i: index of the quint within the block (0-2)
// See section "C.2.12  Integer Sequence Encoding"
uint decodeQuint(uvec4 data, uint offset, uint numBits, uint i) {
    const int inumBits = int(numBits);

    // Note that we don't have the same size issue as trits (see above), since the largest encoding
    // here is 1 quint and 5 bits, which is 3 * 5 + 7 = 22 bits long
    const uint block = extractBits(data, offset, 3 * numBits + 7);

    // Extract the 7 bits that encode the pack of 3 quints
    const uint packedQuints = bitfieldExtract(block, inumBits, 3) |
                              bitfieldExtract(block, 2 * inumBits + 3, 2) << 3 |
                              bitfieldExtract(block, 3 * inumBits + 5, 2) << 5;

    // Extract the LSB
    const ivec3 deltas = {0, 3, 5};
    const uint lowBits = bitfieldExtract(block, int(i) * inumBits + deltas[i], inumBits);

    const uint decoded = kQuintEncodings[packedQuints];
    return bitfieldExtract(decoded, 3 * int(i), 3) << numBits | lowBits;
}

uint decode1Weight(uvec4 weightData, uvec3 encoding, uint numWeights, uint index) {
    if (index >= numWeights) return 0;

    uint numBits = encoding.z;

    if (encoding.x == 1) {
        // 1 trit
        uint offset = (index / 5) * (5 * numBits + 8);
        uint w = decodeTrit(weightData, offset, numBits, index % 5);
        return kUnquantTritWeightMap[3 * ((1 << numBits) - 1) + w];
    } else if (encoding.y == 1) {
        // 1 quint
        uint offset = (index / 3) * (3 * numBits + 7);
        uint w = decodeQuint(weightData, offset, numBits, index % 3);
        return kUnquantQuintWeightMap[5 * ((1 << numBits) - 1) + w];
    } else {
        // only bits, no trits or quints. We can have between 1 and 6 bits.
        uint offset = index * numBits;
        uint w = extractBits(weightData, offset, numBits);

        // The first number in the table is the multiplication factor: 63 / (2^numBits - 1)
        // The second number is a shift factor to adjust when the previous result isn't an integer.
        const uvec2 kUnquantTable[] = {{63, 8}, {21, 8}, {9, 8}, {4, 2}, {2, 4}, {1, 8}};
        const uvec2 unquant = kUnquantTable[numBits - 1];
        w = w * unquant.x | w >> unquant.y;
        if (w > 32) w += 1;
        return w;
    }
}

uint interpolateWeights(uvec4 weightData, uvec3 encoding, uint numWeights, uint index,
                        uint gridWidth, uint stride, uint offset, uvec2 fractionalPart) {
    uvec4 weightIndices = stride * (uvec4(index) + uvec4(0, 1, gridWidth, gridWidth + 1)) + offset;

    // TODO(gregschlom): Optimization idea: instead of always decoding 4 weights, we could decode
    // just what we need depending on whether fractionalPart.x and fractionalPart.y are 0
    uvec4 weights = uvec4(decode1Weight(weightData, encoding, numWeights, weightIndices[0]),
                          decode1Weight(weightData, encoding, numWeights, weightIndices[1]),
                          decode1Weight(weightData, encoding, numWeights, weightIndices[2]),
                          decode1Weight(weightData, encoding, numWeights, weightIndices[3]));

    uint w11 = (fractionalPart.x * fractionalPart.y + 8) >> 4;
    uvec4 factors = uvec4(16 - fractionalPart.x - fractionalPart.y + w11,  // w00
                          fractionalPart.x - w11,                          // w01
                          fractionalPart.y - w11,                          // w10
                          w11);                                            // w11

    return uint(dot(weights, factors) + 8) >> 4;  // this is what the spec calls "effective weight"
}

uvec2 decodeWeights(uvec4 weightData, const uvec2 posInBlock) {
    // Refer to "C.2.18  Weight Infill to interpolate between 4 grid points"

    // TODO(gregschlom): The spec says: "since the block dimensions are constrained, these are
    // easily looked up in a table." - Is it worth doing?
    uvec2 scaleFactor = (1024 + blockSize / 2) / (blockSize - 1);

    uvec2 homogeneousCoords = posInBlock * scaleFactor;
    uvec2 gridCoords = (homogeneousCoords * (weightGridSize - 1) + 32) >> 6;
    uvec2 integralPart = gridCoords >> 4;
    uvec2 fractionalPart = gridCoords & 0xf;

    uint gridWidth = weightGridSize.x;
    uint v0 = integralPart.y * gridWidth + integralPart.x;

    uvec2 weights = uvec2(0);
    weights.x = interpolateWeights(weightData, weightEncoding, numWeights, v0, gridWidth,
                                   1 << int(dualPlane), 0, fractionalPart);
    if (dualPlane) {
        weights.y = interpolateWeights(weightData, weightEncoding, numWeights, v0, gridWidth, 2, 1,
                                       fractionalPart);
    }
    return weights;
}

uint hash52(uint p) {
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}

uint selectPartition(uint seed, uvec2 pos, uint numPartitions) {
    if (numPartitions == 1) {
        return 0;
    }
    if (blockSize.x * blockSize.y < 31) {
        pos <<= 1;
    }
    seed = 1024 * numPartitions + (seed - 1024);
    uint rnum = hash52(seed);
    // TODO(gregschlom): micro-optimization: could repetedly halve the bits to extract them in 6
    // calls to bitfieldExtract instead of 8.
    uvec4 seedA = uvec4(bitfieldExtract(rnum, 0, 4), bitfieldExtract(rnum, 4, 4),
                        bitfieldExtract(rnum, 8, 4), bitfieldExtract(rnum, 12, 4));
    uvec4 seedB = uvec4(bitfieldExtract(rnum, 16, 4), bitfieldExtract(rnum, 20, 4),
                        bitfieldExtract(rnum, 24, 4), bitfieldExtract(rnum, 28, 4));

    seedA = seedA * seedA;
    seedB = seedB * seedB;

    uvec2 shifts1 = uvec2((seed & 2) != 0 ? 4 : 5, numPartitions == 3 ? 6 : 5);
    uvec4 shifts2 = (seed & 1) != 0 ? shifts1.xyxy : shifts1.yxyx;

    seedA >>= shifts2;
    seedB >>= shifts2;

    // Note: this could be implemented as matrix multiplication, but we'd have to use floats and I'm
    // not sure if the values are always small enough to stay accurate.
    uvec4 result =
        uvec4(dot(seedA.xy, pos), dot(seedA.zw, pos), dot(seedB.xy, pos), dot(seedB.zw, pos)) +
        (uvec4(rnum) >> uvec4(14, 10, 6, 2));

    result &= uvec4(0x3F);

    if (numPartitions == 2) {
        result.zw = uvec2(0);
    } else if (numPartitions == 3) {
        result.w = 0;
    }

    // Return the index of the largest component in `result`
    if (all(greaterThanEqual(uvec3(result.x), result.yzw))) {
        return 0;
    } else if (all(greaterThanEqual(uvec2(result.y), result.zw))) {
        return 1;
    } else if (result.z >= result.w) {
        return 2;
    } else {
        return 3;
    }
}

uvec3 getEndpointEncoding(uint availableEndpointBits, uint numEndpoints, out uint actualSize) {
    // This implements the algorithm described in section "C.2.22  Data Size Determination"
    // TODO(gregschlom): This could be implemented with a lookup table instead. Or we could use a
    // binary search but not sure if worth it due to the extra cost of branching.
    for (uint i = 0; i < kColorEncodings.length(); ++i) {
        uvec3 encoding = kColorEncodings[i];
        actualSize = getEncodingSize(numEndpoints, encoding);
        if (actualSize <= availableEndpointBits) {
            return encoding;
        }
    }
    return uvec3(0);  // this should never happen
}

ivec4 blueContract(ivec4 v) { return ivec4((v.r + v.b) >> 1, (v.g + v.b) >> 1, v.ba); }

int sum(ivec3 v) { return v.x + v.y + v.z; }

void bitTransferSigned(inout ivec4 a, inout ivec4 b) {
    b >>= 1;
    b |= a & 0x80;
    a >>= 1;
    a &= 0x3f;
    // This is equivalent to: "if ((a & 0x20) != 0) a -= 0x40;" in the spec. It treats "a" as a
    // 6-bit signed integer, converting it from (0, 63) to (-32, 31)
    a = bitfieldExtract(a, 0, 6);
}

// Decodes the endpoints and writes them to ep0 and ep1.
// vA: even-numbered values in the spec (ie: v0, v2, v4 and v6)
// vB: odd-numbered values in the spec (ie: v1, v3, v5 and v7)
// mode: the CEM (color endpoint mode)
// Note: HDR modes are not supported.
void decodeEndpoints(ivec4 vA, ivec4 vB, uint mode, out uvec4 ep0, out uvec4 ep1) {
    switch (mode) {
        case 0:  // LDR luminance only, direct
            ep0 = uvec4(vA.xxx, 255);
            ep1 = uvec4(vB.xxx, 255);
            return;

        case 1: {  // LDR luminance only, base + offset
            const int l0 = (vA.x >> 2) | (vB.x & 0xC0);
            const int l1 = min(l0 + (vB.x & 0x3F), 255);
            ep0 = uvec4(uvec3(l0), 255);
            ep1 = uvec4(uvec3(l1), 255);
            return;
        }

        case 4:  // LDR luminance + alpha, direct
            ep0 = vA.xxxy;
            ep1 = vB.xxxy;
            return;

        case 5:  // LDR luminance + alpha, base + offset
            bitTransferSigned(vB, vA);
            ep0 = clamp(vA.xxxy, 0, 255);
            ep1 = clamp(vA.xxxy + vB.xxxy, 0, 255);
            return;

        case 6:  // LDR RGB, base + scale
            ep1 = uvec4(vA.x, vB.x, vA.y, 255);
            ep0 = uvec4((ep1.rgb * vB.y) >> 8, 255);
            return;

        case 10:  //  LDR RGB, base + scale, plus alphas
            ep1 = uvec4(vA.x, vB.x, vA.y, vB.z);
            ep0 = uvec4((ep1.rgb * vB.y) >> 8, vA.z);
            return;

        case 8:  // LDR RGB, direct
            vA.a = 255;
            vB.a = 255;
        case 12:  // LDR RGBA, direct
            if (sum(vB.rgb) >= sum(vA.rgb)) {
                ep0 = vA;
                ep1 = vB;
            } else {
                ep0 = blueContract(vB);
                ep1 = blueContract(vA);
            }
            return;

        case 9:  // LDR RGB, base + offset
            // We will end up with vA.a = 255 and vB.a = 0 after calling bitTransferSigned(vB, vA)
            vA.a = 255;
            vB.a = -128;
        case 13:  // LDR RGBA, base + offset
            bitTransferSigned(vB, vA);
            if (sum(vB.rgb) >= 0) {
                ep0 = clamp(vA, 0, 255);
                ep1 = clamp(vA + vB, 0, 255);
            } else {
                ep0 = clamp(blueContract(vA + vB), 0, 255);
                ep1 = clamp(blueContract(vA), 0, 255);
            }
            return;

        default:
            // Unimplemented color encoding. (HDR)
            ep0 = uvec4(0);
            ep1 = uvec4(0);
    }
}

uint decode1Endpoint(uvec4 data, uint startOffset, uint index, uvec3 encoding) {
    uint numBits = encoding.z;

    if (encoding.x == 1) {
        // 1 trit
        uint offset = (index / 5) * (5 * numBits + 8) + startOffset;
        uint ep = decodeTrit(data, offset, numBits, index % 5);
        return kUnquantTritColorMap[3 * ((1 << numBits) - 1) + ep];
    } else if (encoding.y == 1) {
        // 1 quint
        uint offset = (index / 3) * (3 * numBits + 7) + startOffset;
        uint ep = decodeQuint(data, offset, numBits, index % 3);
        return kUnquantQuintColorMap[5 * ((1 << numBits) - 1) + ep];
    } else {
        // only bits, no trits or quints. We can have between 1 and 8 bits.
        uint offset = index * numBits + startOffset;
        uint w = extractBits(data, offset, numBits);
        // The first number in the table is the multiplication factor. 255 / (2^numBits - 1)
        // The second number is a shift factor to adjust when the previous result isn't an integer.
        const uvec2 kUnquantTable[] = {{255, 8}, {85, 8}, {36, 1}, {17, 8},
                                       {8, 2},   {4, 4},  {2, 6},  {1, 8}};
        const uvec2 unquant = kUnquantTable[numBits - 1];
        return w * unquant.x | w >> unquant.y;
    }
}

// Creates a 128-bit mask with the lower n bits set to 1
uvec4 buildBitmask(uint bits) {
    ivec4 numBits = int(bits) - ivec4(96, 64, 32, 0);
    uvec4 mask = (uvec4(1) << clamp(numBits, ivec4(0), ivec4(31))) - 1;
    return mix(mask, uvec4(0xffffffffu), greaterThanEqual(uvec4(bits), uvec4(128, 96, 64, 32)));
}

// Main function to decode the texel at a given position in the block
uvec4 astcDecodeTexel(const uvec2 posInBlock, bool srgb) {
    if (decodeError) {
        return kErrorColor;
    }

    if (voidExtent) {
        return uvec4(bitfieldExtract(astcBlock[1], 0, 16), bitfieldExtract(astcBlock[1], 16, 16),
                     bitfieldExtract(astcBlock[0], 0, 16), bitfieldExtract(astcBlock[0], 16, 16));
    }

    const uvec4 weightData = bitfieldReverse(astcBlock.wzyx) & buildBitmask(weightDataSize);
    const uvec2 weights = decodeWeights(weightData, posInBlock);

    const uint partitionIndex = selectPartition(partitionSeed, posInBlock, numPartitions);

    uint startOfExtraCem = 0;
    uint totalEndpoints = 0;
    uint baseEndpointIndex = 0;
    uint cem = decodeCEM(partitionIndex, startOfExtraCem, totalEndpoints, baseEndpointIndex);

    if (cem == 2u || cem == 3u || cem == 7u || cem == 11u || cem == 14u || cem == 15u) {
        return kErrorColor; // HDR endpoints in an LDR image are the error color.
    }

    // Per spec, we must return the error color if we require more than 18 color endpoints
    if (totalEndpoints > 18) {
        return kErrorColor;
    }

    const uint endpointsStart = (numPartitions == 1) ? 17 : 29;
    const uint endpointsEnd = -2 * int(dualPlane) + startOfExtraCem;
    const uint availableEndpointBits = endpointsEnd - endpointsStart;
    // TODO(gregschlom): Do we need this: if (availableEndpointBits >= 128) return kErrorColor;

    uint actualEndpointBits;
    const uvec3 endpointEncoding =
        getEndpointEncoding(availableEndpointBits, totalEndpoints, actualEndpointBits);
    if (all(equal(endpointEncoding, uvec3(0))) || availableEndpointBits > 128u) return kErrorColor;
    // TODO(gregschlom): Do we need this: if (endpointEncoding == uvec3(0)) return kErrorColor;

    // Number of endpoints pairs in this partition. (Between 1 and 4)
    // This is the n field from "Table C.2.17 - Color Endpoint Modes" divided by two
    const uint numEndpointPairs = (cem >> 2) + 1;

    ivec4 vA = ivec4(0);  // holds what the spec calls v0, v2, v4 and v6
    ivec4 vB = ivec4(0);  // holds what the spec calls v1, v3, v5 and v7

    uvec4 epData = astcBlock & buildBitmask(endpointsStart + actualEndpointBits);

    for (uint i = 0; i < numEndpointPairs; ++i) {
        const uint epIdx = 2 * i + baseEndpointIndex;
        vA[i] = int(decode1Endpoint(epData, endpointsStart, epIdx, endpointEncoding));
        vB[i] = int(decode1Endpoint(epData, endpointsStart, epIdx + 1, endpointEncoding));
    }

    uvec4 ep0, ep1;
    decodeEndpoints(vA, vB, cem, ep0, ep1);

    uvec4 weightsPerChannel = uvec4(weights[0]);
    if (dualPlane) {
        uint ccs = extractBits(astcBlock, endpointsEnd, 2);
        weightsPerChannel[ccs] = weights[1];
    }

    // Khronos Data Format ASTC Weight Application: interpolate expanded 16-bit
    // endpoints. sRGB uses (endpoint << 8) | 0x80, then takes the high byte.
    // Linear uses bit replication and decodes UNORM16 to FP16 (handled by caller).
    ep0 = srgb ? ((ep0 << 8) | 0x80u) : ep0 * 257u;
    ep1 = srgb ? ((ep1 << 8) | 0x80u) : ep1 * 257u;
    return (ep0 * (64u - weightsPerChannel) + ep1 * weightsPerChannel + 32u) >> 6;
}
