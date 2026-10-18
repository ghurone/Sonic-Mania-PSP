#pragma once
// A rotozoom scanline walks the layer from position by deform per pixel (16.16, wrapping at 32 bits): pixel k samples
// position + k * deform. PSPRotoAxis finds, along one axis, the pixels at which that walk enters the next 16-pixel
// tile cell, exactly and without a division per cell: crossing j happens at k_j = ceil(N_j / a), with N_j the distance
// to the j-th cell edge ahead and a the step; since N_{j+1} = N_j + 2^20 = q * a + r more, k_{j+1} follows from k_j
// and the slack e_j = k_j * a - N_j alone.
struct PSPRotoAxis {
    uint32 next; // pixel of the next crossing (0xFFFFFFFF: never)
    uint32 a, q, r, e;
    int32 step; // the cell index change at a crossing

    inline void Init(uint32 pos, int32 d)
    {
        if (!d) {
            next = 0xFFFFFFFF;
            step = 0;
            return;
        }
        a           = d < 0 ? (uint32)-d : (uint32)d;
        step        = d < 0 ? -1 : 1;
        uint32 dist = d > 0 ? 0x100000 - (pos & 0xFFFFF) : (pos & 0xFFFFF) + 1;
        next        = (dist + a - 1) / a; // (dist <= 2^20 and a < 2^31: no overflow, no 64-bit division)
        e           = next * a - dist;
        q           = 0x100000 / a;
        r           = 0x100000 % a;
    }

    inline void Advance()
    {
        if (r > e) {
            next += q + 1;
            e = a - (r - e);
        }
        else {
            next += q;
            e -= r;
        }
    }
};
