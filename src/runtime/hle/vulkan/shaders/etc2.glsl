// Copyright 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.



precision highp int;

const uint VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK = 147;
const uint VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK = 148;
const uint VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK = 149;
const uint VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK = 150;
const uint VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK = 151;
const uint VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK = 152;
const uint VK_FORMAT_EAC_R11_UNORM_BLOCK = 153;
const uint VK_FORMAT_EAC_R11_SNORM_BLOCK = 154;
const uint VK_FORMAT_EAC_R11G11_UNORM_BLOCK = 155;
const uint VK_FORMAT_EAC_R11G11_SNORM_BLOCK = 156;

const int kLookup[8] = {0, 1, 2, 3, -4, -3, -2, -1};

const ivec4 kRGBModifierTable[] = {
    /* 0 */ {2, 8, -2, -8},
    /* 1 */ {5, 17, -5, -17},
    /* 2 */ {9, 29, -9, -29},
    /* 3 */ {13, 42, -13, -42},
    /* 4 */ {18, 60, -18, -60},
    /* 5 */ {24, 80, -24, -80},
    /* 6 */ {33, 106, -33, -106},
    /* 7 */ {47, 183, -47, -183}};

const ivec4 kRGBOpaqueModifierTable[] = {
    /* 0 */ {0, 8, 0, -8},
    /* 1 */ {0, 17, 0, -17},
    /* 2 */ {0, 29, 0, -29},
    /* 3 */ {0, 42, 0, -42},
    /* 4 */ {0, 60, 0, -60},
    /* 5 */ {0, 80, 0, -80},
    /* 6 */ {0, 106, 0, -106},
    /* 7 */ {0, 183, 0, -183}};

const ivec4 kAlphaModifierTable[] = {
    /* 0 */ {-3, -6, -9, -15},  {2, 5, 8, 14},
    /* 1 */ {-3, -7, -10, -13}, {2, 6, 9, 12},
    /* 2 */ {-2, -5, -8, -13},  {1, 4, 7, 12},
    /* 3 */ {-2, -4, -6, -13},  {1, 3, 5, 12},
    /* 4 */ {-3, -6, -8, -12},  {2, 5, 7, 11},
    /* 5 */ {-3, -7, -9, -11},  {2, 6, 8, 10},
    /* 6 */ {-4, -7, -8, -11},  {3, 6, 7, 10},
    /* 7 */ {-3, -5, -8, -11},  {2, 4, 7, 10},
    /* 8 */ {-2, -6, -8, -10},  {1, 5, 7, 9},
    /* 9 */ {-2, -5, -8, -10},  {1, 4, 7, 9},
    /* 10 */ {-2, -4, -8, -10}, {1, 3, 7, 9},
    /* 11 */ {-2, -5, -7, -10}, {1, 4, 6, 9},
    /* 12 */ {-3, -4, -7, -10}, {2, 3, 6, 9},
    /* 13 */ {-1, -2, -3, -10}, {0, 1, 2, 9},
    /* 14 */ {-4, -6, -8, -9},  {3, 5, 7, 8},
    /* 15 */ {-3, -5, -7, -9},  {2, 4, 6, 8}};

bool isOverflowed(uint base, uint diff) {
    int val = int(0x1f & base) + kLookup[0x7 & diff];
    return (val < 0) || (val >= 32);
}

uint convert4To8(uint b) {
    uint c = b & 0xf;
    return (c << 4) | c;
}

uint convert5To8(uint b) {
    uint c = b & 0x1f;
    return (c << 3) | (c >> 2);
}

uint convert6To8(uint b) {
    uint c = b & 0x3f;
    return (c << 2) | (c >> 4);
}

uint convert7To8(uint b) {
    uint c = b & 0x7f;
    return (c << 1) | (c >> 6);
}

uint convertDiff(uint base, uint diff) {
    return convert5To8(uint(int(0x1f & base) + kLookup[0x7 & diff]));
}

int _clamp(int x) { return int(clamp(x, 0, 255)); }

ivec3 _clamp(ivec3 x) { return ivec3(clamp(x, 0, 255)); }

ivec4[16] etc2_T_H_index(ivec3[4] clrTable, uint low, bool isPunchthroughAlpha, bool opaque) {
    ivec4 ret[16];
    for (uint y = 0; y < 4; y++) {
        for (uint x = 0; x < 4; x++) {
            uint k = y + x * 4;
            uint msb = (low >> (k + 15)) & 2;
            uint lsb = (low >> k) & 1;
            if (isPunchthroughAlpha && (!opaque) && (msb != 0) && (lsb == 0)) {
                // rgba all 0
                ret[y * 4 + x] = ivec4(0, 0, 0, 0);
            } else {
                uint offset = lsb | msb;
                ret[y * 4 + x] = ivec4(clrTable[offset], 255);
            }
        }
    }
    return ret;
}

ivec4[16] etc2_decode_block_T(uint high, uint low, bool isPunchthroughAlpha, bool opaque) {
    const int LUT[] = {3, 6, 11, 16, 23, 32, 41, 64};
    int r1, r2, g1, g2, b1, b2;
    r1 = int(convert4To8((((high >> 27) & 3) << 2) | ((high >> 24) & 3)));
    g1 = int(convert4To8(high >> 20));
    b1 = int(convert4To8(high >> 16));
    r2 = int(convert4To8(high >> 12));
    g2 = int(convert4To8(high >> 8));
    b2 = int(convert4To8(high >> 4));
    // 3 bits intense modifier
    int intenseIdx = int((((high >> 2) & 3) << 1) | (high & 1));
    int intenseMod = LUT[intenseIdx];
    ivec3 clrTable[4];
    clrTable[0] = ivec3(r1, g1, b1);
    clrTable[1] = ivec3(_clamp(int(r2) + intenseMod), _clamp(int(g2) + intenseMod),
                        _clamp(int(b2) + intenseMod));
    clrTable[2] = ivec3(r2, g2, b2);
    clrTable[3] = ivec3(_clamp(int(r2) - intenseMod), _clamp(int(g2) - intenseMod),
                        _clamp(int(b2) - intenseMod));
    return etc2_T_H_index(clrTable, low, isPunchthroughAlpha, opaque);
}

ivec4[16] etc2_decode_block_H(uint high, uint low, bool isPunchthroughAlpha, bool opaque) {
    const int LUT[] = {3, 6, 11, 16, 23, 32, 41, 64};
    ivec3 rgb1, rgb2;
    rgb1.r = int(convert4To8(high >> 27));
    rgb1.g = int(convert4To8(((high >> 24) << 1) | ((high >> 20) & 1)));
    rgb1.b = int(convert4To8(((high >> 19) << 3) | ((high >> 15) & 7)));
    rgb2.r = int(convert4To8(high >> 11));
    rgb2.g = int(convert4To8(high >> 7));
    rgb2.b = int(convert4To8(high >> 3));
    // 3 bits intense modifier
    uint intenseIdx = high & 4;
    intenseIdx |= (high & 1) << 1;
    intenseIdx |= uint(((rgb1.r << 16) | (rgb1.g << 8) | rgb1.b) >=
                       ((rgb2.r << 16) | (rgb2.g << 8) | rgb2.b));
    int intenseMod = LUT[intenseIdx];
    ivec3 clrTable[4];
    clrTable[0] = _clamp(ivec3(rgb1) + intenseMod);
    clrTable[1] = _clamp(ivec3(rgb1) - intenseMod);
    clrTable[2] = _clamp(ivec3(rgb2) + intenseMod);
    clrTable[3] = _clamp(ivec3(rgb2) - intenseMod);
    return etc2_T_H_index(clrTable, low, isPunchthroughAlpha, opaque);
}

ivec4[16] etc2_decode_block_P(uint high, uint low, bool isPunchthroughAlpha) {
    ivec3 rgbo, rgbh, rgbv;
    rgbo.r = int(convert6To8(high >> 25));
    rgbo.g = int(convert7To8(((high >> 24) << 6) | ((high >> 17) & 63)));
    rgbo.b = int(convert6To8(((high >> 16) << 5) | (((high >> 11) & 3) << 3) | ((high >> 7) & 7)));
    rgbh.r = int(convert6To8(((high >> 2) << 1) | (high & 1)));
    rgbh.g = int(convert7To8(low >> 25));
    rgbh.b = int(convert6To8(low >> 19));
    rgbv.r = int(convert6To8(low >> 13));
    rgbv.g = int(convert7To8(low >> 6));
    rgbv.b = int(convert6To8(low));
    ivec4 ret[16];
    for (int i = 0; i < 16; i++) {
        int y = i >> 2;
        int x = i & 3;
        ret[i] = ivec4(_clamp((x * (rgbh - rgbo) + y * (rgbv - rgbo) + 4 * rgbo + 2) >> 2), 255);
        ret[i].a = 255;
    }
    return ret;
}

void decode_subblock(inout ivec4 pOut[16], int r, int g, int b, ivec4 table, uint low, bool second,
                     bool flipped, bool isPunchthroughAlpha, bool opaque) {
    uint baseX = 0;
    uint baseY = 0;
    if (second) {
        if (flipped) {
            baseY = 2;
        } else {
            baseX = 2;
        }
    }
    for (int i = 0; i < 8; i++) {
        uint x, y;
        if (flipped) {
            x = baseX + (i >> 1);
            y = baseY + (i & 1);
        } else {
            x = baseX + (i >> 2);
            y = baseY + (i & 3);
        }
        uint k = y + (x * 4);
        uint msb = ((low >> (k + 15)) & 2);
        uint lsb = ((low >> k) & 1);
        uint q = x + 4 * y;
        if (isPunchthroughAlpha && (!opaque) && (msb != 0) && (lsb == 0)) {
            // rgba all 0
            pOut[q] = ivec4(0, 0, 0, 0);
        } else {
            uint offset = lsb | msb;
            int delta = table[offset];
            pOut[q] =
                ivec4(_clamp(int(r) + delta), _clamp(int(g) + delta), _clamp(int(b) + delta), 255);
        }
    }
}

ivec4[16] allZeros() {
    ivec4[16] ret;
    for (int i = 0; i < 16; i++) {
        ret[i] = ivec4(0);
    }
    return ret;
}

ivec4[16] etc2_decode_rgb_block(uint high, uint low, bool isPunchthroughAlpha) {
    bool opaque = (((high >> 1) & 1) != 0);
    int r1, r2, g1, g2, b1, b2;
    if (isPunchthroughAlpha || ((high & 2) != 0)) {
        // differntial
        uint rBase = high >> 27;
        uint gBase = high >> 19;
        uint bBase = high >> 11;
        if (isOverflowed(rBase, high >> 24)) {
            return etc2_decode_block_T(high, low, isPunchthroughAlpha, opaque);
        }
        if (isOverflowed(gBase, high >> 16)) {
            return etc2_decode_block_H(high, low, isPunchthroughAlpha, opaque);
        }
        if (isOverflowed(bBase, high >> 8)) {
            return etc2_decode_block_P(high, low, isPunchthroughAlpha);
        }
        r1 = int(convert5To8(rBase));
        r2 = int(convertDiff(rBase, high >> 24));
        g1 = int(convert5To8(gBase));
        g2 = int(convertDiff(gBase, high >> 16));
        b1 = int(convert5To8(bBase));
        b2 = int(convertDiff(bBase, high >> 8));
    } else {
        // not differential
        r1 = int(convert4To8(high >> 28));
        r2 = int(convert4To8(high >> 24));
        g1 = int(convert4To8(high >> 20));
        g2 = int(convert4To8(high >> 16));
        b1 = int(convert4To8(high >> 12));
        b2 = int(convert4To8(high >> 8));
    }
    uint tableIndexA = 7 & (high >> 5);
    uint tableIndexB = 7 & (high >> 2);
    ivec4 tableA;
    ivec4 tableB;
    if (opaque || !isPunchthroughAlpha) {
        tableA = kRGBModifierTable[tableIndexA];
        tableB = kRGBModifierTable[tableIndexB];
    } else {
        tableA = kRGBOpaqueModifierTable[tableIndexA];
        tableB = kRGBOpaqueModifierTable[tableIndexB];
    }
    bool flipped = ((high & 1) != 0);
    ivec4[16] ret;
    decode_subblock(ret, r1, g1, b1, tableA, low, false, flipped, isPunchthroughAlpha, opaque);
    decode_subblock(ret, r2, g2, b2, tableB, low, true, flipped, isPunchthroughAlpha, opaque);
    return ret;
}

uint[16] eac_decode_single_channel_block(uint high, uint low, bool isSigned) {
    int base_codeword = int(high >> 24);
    base_codeword &= 255;
    int multiplier = int(high >> 20);
    multiplier &= 15;

    uint tblIdx = ((high >> 16) & 15);
    const ivec4 table0 = kAlphaModifierTable[tblIdx * 2];
    const ivec4 table1 = kAlphaModifierTable[tblIdx * 2 + 1];
    const uint p[16] = {
        high >> 13, high >> 10, high >> 7, high >> 4, high >> 1, (high << 2) | (low >> 30),
        low >> 27,  low >> 24,  low >> 21, low >> 18, low >> 15, low >> 12,
        low >> 9,   low >> 6,   low >> 3,  low};
    uint result[16];
    for (uint i = 0; i < 16; i++) {
        // flip x, y in output
        uint outIdx = (i % 4) * 4 + i / 4;

        uint modifier = (p[i] & 7);
        int modifierValue = ((modifier >= 4) ? table1[modifier - 4] : table0[modifier]);
        int decoded = base_codeword + modifierValue * multiplier;
        result[outIdx] = uint(_clamp(decoded));
    }
    return result;
}

float[16] eac_decode_single_channel_block_float(uint high, uint low, bool isSigned) {
    int base_codeword = int(high >> 24);
    if (isSigned) {
        if (base_codeword >= 128) {
            base_codeword -= 256;
        }
        if (base_codeword == -128) {
            base_codeword = -127;
        }
    }
    int multiplier = int(high >> 20);
    multiplier &= 15;

    uint tblIdx = ((high >> 16) & 15);
    const ivec4 table0 = kAlphaModifierTable[tblIdx * 2];
    const ivec4 table1 = kAlphaModifierTable[tblIdx * 2 + 1];
    const uint p[16] = {
        high >> 13, high >> 10, high >> 7, high >> 4, high >> 1, (high << 2) | (low >> 30),
        low >> 27,  low >> 24,  low >> 21, low >> 18, low >> 15, low >> 12,
        low >> 9,   low >> 6,   low >> 3,  low};
    float result[16];
    for (uint i = 0; i < 16; i++) {
        // flip x, y in output
        uint outIdx = (i % 4) * 4 + i / 4;

        uint modifier = (p[i] & 7);
        int modifierValue = ((modifier >= 4) ? table1[modifier - 4] : table0[modifier]);
        int decoded = base_codeword + modifierValue * multiplier;
        decoded *= 8;
        if (multiplier == 0) {
            decoded += modifierValue;
        }
        if (isSigned) {
            decoded = clamp(decoded, -1023, 1023);
            result[outIdx] = float(decoded) / 1023.0;
        } else {
            decoded += 4;
            decoded = clamp(decoded, 0, 2047);
            result[outIdx] = float(decoded) / 2047.0;
        }
    }
    return result;
}

uint constructUint32(uint a16, uint b16) {
    uint a2 = (a16 & 0xff) << 8;
    a2 |= (a16 >> 8) & 0xff;
    uint b2 = (b16 & 0xff) << 8;
    b2 |= (b16 >> 8) & 0xff;
    return (a2 << 16) | b2;
}

uint flip32(uint a) {
    return ((a & 0xff) << 24) | ((a & 0xff00) << 8) | ((a & 0xff0000) >> 8) |
           ((a & 0xff000000) >> 24);
}

#define BLOCK_Y_SIZE_1DArray 1
#define BLOCK_Y_SIZE_2DArray 4
#define BLOCK_Y_SIZE_3D 4
