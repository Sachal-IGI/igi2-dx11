/* dev: terrain hole scan (nx_terrain_scan.txt in the game folder; dx11_terrain.c, Direct3D 11 mode with the GPU terrain).
   Once a level's terrain has been drawn for a while, the camera tours views made from its height maps: along every border
   of a map that lies in another (close from both sides, far, and grazing along it), at each map's steepest ground (close
   and far) and at random spots low (eye height) and high. Every view looks level (no pitch, no roll), so each
   screen column is a vertical plane through the eye, and along it the distance to an unbroken height field can only
   grow going up the screen. Only the terrain is drawn (on magenta; the DiscardTask holes, tunnels and shafts open on
   purpose, closed; no near or far clipping), and the depth buffer is read back. In a column, nothing drawn below
   ground that is drawn is a sky hole (the sky shows through the terrain); ground drawn farther than ground above it
   is see-through (a gap that shows ground behind it: also where two maps overlap and the game itself draws both). Each view with holes is logged with its
   nx_view line (to rebuild it with nx_view.txt) and the first ones are written as BMP to nx_scan\ in the game
   folder (hole pixels painted magenta); at the end a summary line. Optional words in the file:
   "views N" (per map, default 96), "hold F" (frames per view, default 2), "bmp B" (pictures, default 24), "min P" (hole
   pixels that count, default 3), "seed S", "quit 1" (close the game when done), "view ex ey ez ax ay az" (a view of
   one's own in nx_view.txt units, up to 16, scanned first). Without the file nothing of this runs. */

int nx_scan_active;                 /* D_Clear: magenta; DrawGPU: DiscardTask holes off */

enum { SK_NEAR_IN, SK_NEAR_OUT, SK_FAR, SK_ALONG, SK_STEEP_NEAR, SK_STEEP_FAR, SK_LOW, SK_HIGH, SK_GIVEN, SK_COUNT };
static const char *const kScanKind[SK_COUNT] = { "border, inside", "border, outside", "border, far", "border, along",
                                                 "steep, near", "steep, far", "low", "high", "given" };
typedef struct ScanView { float e[3], a[3]; short map, kind; } ScanView;
#define SCAN_MAX 3000
static ScanView *g_scan_views;
static MapInfo g_scan_mi[MAX_MAPS];
static int g_scan_nmaps, g_scan_n, g_scan_i = -1, g_scan_hold = 2, g_scan_bmp = 24, g_scan_min = 3, g_scan_per_map = 96;
static int g_scan_written, g_scan_hit_views, g_scan_settle;
static unsigned g_scan_set_frame, g_scan_rand = 12345u;
static BOOL g_scan_checked, g_scan_on, g_scan_done, g_scan_quit;
static float g_scan_given[16][6];
static int g_scan_ngiven;
static double g_scan_pixels, g_scan_through;
static int g_scan_through_views;
static long g_scan_kind_hits[SK_COUNT];

static float ScanRand(void) { g_scan_rand = g_scan_rand * 1664525u + 1013904223u; return (float)(g_scan_rand >> 8) / 16777216.0f; }

/* the ground at (x, y) for an eye: the highest corner of its cell in every map that holds it (a triangle never rises
   above its corners; the maps there may overlap) */
static float ScanGround(float x, float y)
{
    int k;
    float best = -3e38f;
    for (k = g_scan_nmaps - 1; k >= 0; --k) {
        const MapInfo *mk = &g_scan_mi[k];
        const TMap *m = &T.map[k];
        const float *hp = (const float *)m->heights;
        float fx, fy;
        int i, j;
        if (!mk->used || !hp || m->w < 2 || m->h < 2) continue;
        if (x < mk->region[0] || x > mk->region[1] || y < mk->region[2] || y > mk->region[3]) continue;
        fx = (x - mk->x0) / mk->cx; fy = (y - mk->y0) / mk->cy;
        i = (int)floorf(fx); j = (int)floorf(fy);
        i = i < 0 ? 0 : i > m->w - 2 ? m->w - 2 : i;
        j = j < 0 ? 0 : j > m->h - 2 ? m->h - 2 : j;
        if (!Readable(hp + (size_t)j * m->w + i, (size_t)(m->w + 2) * sizeof(float))) continue;
        {
            float a = hp[(size_t)j * m->w + i], b = hp[(size_t)j * m->w + i + 1];
            float c = hp[(size_t)(j + 1) * m->w + i], d = hp[(size_t)(j + 1) * m->w + i + 1];
            float hi = a > b ? a : b;
            hi = hi > c ? hi : c; hi = hi > d ? hi : d;
            if (hi > best) best = hi;
        }
    }
    return best > -1e38f ? best : 0;
}

/* the highest ground within r of (x, y): an eye that clears the hills around it */
static float ScanGroundMax(float x, float y, float r)
{
    float z = ScanGround(x, y);
    int i;
    for (i = 0; i < 12; ++i) {
        float a = (float)i * 0.5235988f, h = ScanGround(x + r * cosf(a), y + r * sinf(a));
        if (h > z) z = h;
    }
    return z;
}

static float g_scan_jitter;   /* a fraction of the finest cell: eyes off the grid lines, as in play */

/* TRUE when (x, y) lies in a map (an eye outside every map would stand under the world) */
static BOOL ScanInside(float x, float y)
{
    int k;
    for (k = 0; k < g_scan_nmaps; ++k)
        if (g_scan_mi[k].used && x >= g_scan_mi[k].region[0] && x <= g_scan_mi[k].region[1] &&
            y >= g_scan_mi[k].region[2] && y <= g_scan_mi[k].region[3]) return TRUE;
    return FALSE;
}

static void ScanAdd(int map, int kind, float ex, float ey, float ez, float ax, float ay, float az)
{
    ScanView *v;
    float dx, dy;
    if (g_scan_n >= SCAN_MAX) return;
    if (kind != SK_GIVEN) {
        /* an eye exactly on a grid line looks along the mesh's edges: a seam thinner than a pixel then lines up into a
           whole column, which no player sees; a small random offset, the same for eye and target */
        float jx = (ScanRand() - 0.5f) * g_scan_jitter, jy = (ScanRand() - 0.5f) * g_scan_jitter;
        ex += jx; ey += jy; ax += jx; ay += jy;
        if (!ScanInside(ex, ey)) return;
    }
    dx = ax - ex; dy = ay - ey;
    if (dx * dx + dy * dy < 1e-6f) ax += 1.0f;   /* never straight down: the view's right vector comes from xy */
    v = &g_scan_views[g_scan_n++];
    v->e[0] = ex; v->e[1] = ey; v->e[2] = ez; v->a[0] = ax; v->a[1] = ay; v->a[2] = az;
    v->map = (short)map; v->kind = (short)kind;
}

static void ScanMakeViews(void)
{
    int k, n0;
    float cf = 1e30f;
    for (k = 0; k < g_scan_nmaps; ++k) if (g_scan_mi[k].used && g_scan_mi[k].cx < cf) cf = g_scan_mi[k].cx;
    if (!(cf < 1e29f)) return;
    g_scan_jitter = 0.6f * cf;
    for (k = 0; k < g_scan_nmaps; ++k) {
        const MapInfo *mk = &g_scan_mi[k];
        const TMap *m = &T.map[k];
        const float *hp = (const float *)m->heights;
        float x0 = mk->region[0], x1 = mk->region[1], y0 = mk->region[2], y1 = mk->region[3];
        float inset = 0.1f * ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0));
        int budget = g_scan_per_map, e, p, i;
        if (!mk->used) continue;
        n0 = g_scan_n;
        /* borders of a map inside another (the outer map's own border is the world's edge) */
        if (mk->parent >= 0) {
            for (e = 0; e < 4; ++e)
                for (p = 0; p < 4; ++p) {
                    float f = ((float)p + 0.5f) / 4.0f, bx, by, nx, ny, tx, ty;
                    if (e == 0) { bx = x0; by = y0 + f * (y1 - y0); nx = 1; ny = 0; }
                    else if (e == 1) { bx = x1; by = y0 + f * (y1 - y0); nx = -1; ny = 0; }
                    else if (e == 2) { bx = x0 + f * (x1 - x0); by = y0; nx = 0; ny = 1; }
                    else { bx = x0 + f * (x1 - x0); by = y1; nx = 0; ny = -1; }
                    tx = -ny; ty = nx;
                    {
                        float ex = bx + nx * 15 * cf, ey = by + ny * 15 * cf;
                        { float ez = ScanGroundMax(ex, ey, 3 * cf) + 5 * cf; ScanAdd(k, SK_NEAR_IN, ex, ey, ez, bx, by, ez); }
                        ex = bx - nx * 15 * cf; ey = by - ny * 15 * cf;
                        { float ez = ScanGroundMax(ex, ey, 3 * cf) + 5 * cf; ScanAdd(k, SK_NEAR_OUT, ex, ey, ez, bx, by, ez); }
                        ex = bx + (p & 1 ? 1 : -1) * nx * 140 * cf; ey = by + (p & 1 ? 1 : -1) * ny * 140 * cf;
                        { float ez = ScanGroundMax(ex, ey, 6 * cf) + 30 * cf; ScanAdd(k, SK_FAR, ex, ey, ez, bx, by, ez); }
                        ex = bx + tx * 30 * cf + nx * 2 * cf; ey = by + ty * 30 * cf + ny * 2 * cf;
                        {
                            float ez = ScanGroundMax(ex, ey, 3 * cf) + 3 * cf;
                            ScanAdd(k, SK_ALONG, ex, ey, ez, bx - tx * 30 * cf, by - ty * 30 * cf, ez);
                        }
                    }
                }
        }
        /* the steepest ground: the largest rise over two cells, spread out (32 cells apart) */
        if (hp && m->w > 4 && m->h > 4 && Readable(hp, (size_t)m->w * m->h * sizeof(float))) {
            enum { NSTEEP = 8 };
            float best[NSTEEP]; int bi[NSTEEP], bj[NSTEEP], nb = 0, s, j, step = m->w * m->h > 512 * 512 ? 2 : 1;
            for (j = 2; j < m->h - 2; j += step)
                for (i = 2; i < m->w - 2; i += step) {
                    float c = hp[(size_t)j * m->w + i];
                    float g = fabsf(hp[(size_t)j * m->w + i + 2] - c) + fabsf(hp[(size_t)(j + 2) * m->w + i] - c);
                    int q, slot = -1;
                    for (q = 0; q < nb; ++q)
                        if (abs(bi[q] - i) < 32 && abs(bj[q] - j) < 32) { slot = best[q] < g ? q : -2; break; }
                    if (slot == -2) continue;
                    if (slot < 0) {
                        if (nb < NSTEEP) slot = nb++;
                        else { for (q = 1, slot = 0; q < nb; ++q) if (best[q] < best[slot]) slot = q; if (best[slot] >= g) continue; }
                    }
                    best[slot] = g; bi[slot] = i; bj[slot] = j;
                }
            for (s = 0; s < nb; ++s) {
                float px = mk->x0 + (float)bi[s] * mk->cx, py = mk->y0 + (float)bj[s] * mk->cy, pz = ScanGround(px, py);
                float gx = ScanGround(px + mk->cx, py) - ScanGround(px - mk->cx, py), gy = ScanGround(px, py + mk->cy) - ScanGround(px, py - mk->cy);
                float gl = sqrtf(gx * gx + gy * gy), dx, dy, ex, ey;
                if (gl <= 0) { gx = 1; gy = 0; gl = 1; }
                dx = -gx / gl; dy = -gy / gl;   /* downhill */
                ex = px + dx * 25 * cf; ey = py + dy * 25 * cf;
                { float ez = ScanGroundMax(ex, ey, 3 * cf) + 5 * cf; ScanAdd(k, SK_STEEP_NEAR, ex, ey, ez, px, py, ez); }
                ex = px + dx * 130 * cf - dy * 40 * cf; ey = py + dy * 130 * cf + dx * 40 * cf;
                { float ez = ScanGroundMax(ex, ey, 6 * cf) + 30 * cf; ScanAdd(k, SK_STEEP_FAR, ex, ey, ez, px, py, ez); }
                (void)pz;
            }
        }
        /* random spots, low (eye height, looking along the ground) and high (looking down at a distance) */
        while (g_scan_n - n0 < budget && g_scan_n < SCAN_MAX) {
            float ex = x0 + inset + ScanRand() * (x1 - x0 - 2 * inset), ey = y0 + inset + ScanRand() * (y1 - y0 - 2 * inset);
            float a = ScanRand() * 6.2831853f, dx = cosf(a), dy = sinf(a);
            if (ScanRand() < 0.5f) {
                float ez = ScanGroundMax(ex, ey, 0.5f * cf) + 1.0f * cf;
                ScanAdd(k, SK_LOW, ex, ey, ez, ex + dx * 60 * cf, ey + dy * 60 * cf, ez);
            } else {
                float ez = ScanGroundMax(ex, ey, 4 * cf) + 40 * cf;
                ScanAdd(k, SK_HIGH, ex, ey, ez, ex + dx * 160 * cf, ey + dy * 160 * cf, ez);
            }
        }
    }
}

/* DrawGPU, after the maps of this frame are known */
static void NxScan_Maps(const MapInfo *mi, int nmaps)
{
    if (!g_scan_checked) {
        char path[MAX_PATH + 32], word[32];
        FILE *f;
        int v;
        g_scan_checked = TRUE;
        snprintf(path, sizeof path, "%s\\nx_terrain_scan.txt", Renderer_GameFolder());
        if (!(f = fopen(path, "r"))) return;
        while (fscanf(f, "%31s", word) == 1) {
            if (!_stricmp(word, "view")) {
                float *g = g_scan_given[g_scan_ngiven < 16 ? g_scan_ngiven : 15];
                if (fscanf(f, "%f %f %f %f %f %f", &g[0], &g[1], &g[2], &g[3], &g[4], &g[5]) != 6) break;
                if (g_scan_ngiven < 16) ++g_scan_ngiven;
                continue;
            }
            if (fscanf(f, "%d", &v) != 1) break;
            if (!_stricmp(word, "quit")) g_scan_quit = v != 0;
            else if (!_stricmp(word, "views") && v > 0 && v <= 1000) g_scan_per_map = v;
            else if (!_stricmp(word, "hold") && v >= 1 && v <= 60) g_scan_hold = v;
            else if (!_stricmp(word, "bmp") && v >= 0 && v <= 500) g_scan_bmp = v;
            else if (!_stricmp(word, "min") && v >= 0) g_scan_min = v;   /* 0: every view counts (to look at them) */
            else if (!_stricmp(word, "seed")) g_scan_rand = (unsigned)v;
        }
        fclose(f);
        g_scan_on = TRUE;
        nx_scan_active = 1;
        nx_dev_terrain_only = 1;
        Nx_Log("terrain scan: on (views per map %d, hold %d frames, min %d hole pixels)", g_scan_per_map, g_scan_hold, g_scan_min);
    }
    if (!g_scan_on || g_scan_views || g_scan_done || nmaps <= 0) return;
    if (++g_scan_settle < 90) return;   /* the level's first frames: let the game settle */
    g_scan_views = (ScanView *)calloc(SCAN_MAX, sizeof(ScanView));
    if (!g_scan_views) { g_scan_done = TRUE; return; }
    g_scan_nmaps = nmaps > MAX_MAPS ? MAX_MAPS : nmaps;
    memcpy(g_scan_mi, mi, sizeof(MapInfo) * g_scan_nmaps);
    {
        float s = F32(0x853EFF0u, 0);
        int i;
        if (s == 0) s = 1;
        for (i = 0; i < g_scan_ngiven; ++i) {
            const float *g = g_scan_given[i];
            ScanAdd(-1, SK_GIVEN, g[0] / s, g[1] / s, g[2] / s, g[3] / s, g[4] / s, g[5] / s);
        }
    }
    ScanMakeViews();
    {
        int k;
        for (k = 0; k < g_scan_nmaps; ++k)
            Nx_Log("terrain scan: map %d %dx%d cell %g parent %d region %g %g %g %g", k, T.map[k].w, T.map[k].h, g_scan_mi[k].cx,
                   g_scan_mi[k].parent, g_scan_mi[k].region[0], g_scan_mi[k].region[1], g_scan_mi[k].region[2], g_scan_mi[k].region[3]);
    }
    Nx_Log("terrain scan: %d views", g_scan_n);
    if (!g_scan_n) g_scan_done = TRUE;
}

/* the depth of the frame as view-space distance (view units) per pixel; nothing drawn = 3e38. FALSE: not readable */
static float *g_scan_z;
static BYTE *g_scan_mask;
static UINT g_scan_zw, g_scan_zh;
static BOOL ScanReadDepth(NxDevice *dev)
{
    NxSurface *zs = dev->zs ? dev->zs : dev->depth;
    UINT sub = 0, x, y;
    ID3D11Texture2D *src, *staging = NULL;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    float p33 = dev->s.proj._33, p43 = dev->s.proj._43;
    int kind;
    BOOL ok = FALSE;
    if (!zs || !(src = Nx_SurfaceTexture(zs, &sub))) return FALSE;
    NxQ_Sync(dev);   /* everything drawn so far has run; the context is this thread's until it records again */
    ID3D11Texture2D_GetDesc(src, &d);
    kind = d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_R24G8_TYPELESS ? 1
         : d.Format == DXGI_FORMAT_D32_FLOAT || d.Format == DXGI_FORMAT_R32_TYPELESS ? 2
         : d.Format == DXGI_FORMAT_D16_UNORM || d.Format == DXGI_FORMAT_R16_TYPELESS ? 3 : 0;
    if (!kind || d.SampleDesc.Count > 1 || p33 == 0) return FALSE;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev->d, &d, NULL, &staging))) return FALSE;
    ID3D11DeviceContext_CopySubresourceRegion(dev->c, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)src, sub, NULL);
    if (SUCCEEDED(ID3D11DeviceContext_Map(dev->c, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        if (d.Width != g_scan_zw || d.Height != g_scan_zh) {
            free(g_scan_z); free(g_scan_mask);
            g_scan_z = (float *)malloc(sizeof(float) * d.Width * d.Height);
            g_scan_mask = (BYTE *)malloc((size_t)d.Width * d.Height);
            g_scan_zw = g_scan_z && g_scan_mask ? d.Width : 0; g_scan_zh = g_scan_z && g_scan_mask ? d.Height : 0;
        }
        if (g_scan_zw) {
            for (y = 0; y < d.Height; ++y) {
                const BYTE *row = (const BYTE *)m.pData + (size_t)y * m.RowPitch;
                float *out = g_scan_z + (size_t)y * d.Width;
                for (x = 0; x < d.Width; ++x) {
                    float z;
                    if (kind == 1) z = (float)(((const UINT *)row)[x] & 0xFFFFFFu) / 16777215.0f;
                    else if (kind == 2) z = ((const float *)row)[x];
                    else z = (float)((const WORD *)row)[x] / 65535.0f;
                    /* d = p33 + p43 / v (the D3D projection): v = p43 / (d - p33); the cleared far value is "nothing" */
                    out[x] = z >= 0.9999995f ? 3e38f : p43 / (z - p33);
                }
            }
            ok = TRUE;
        }
        ID3D11DeviceContext_Unmap(dev->c, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return ok;
}

/* the frame of view i, per column from the top: nothing drawn below drawn ground is a sky hole; ground farther than
   ground above it (3 % and 2 view units of slack) is see-through */
static void ScanAnalyze(NxDevice *dev, int vi)
{
    NxSurface *b = dev->backbuffer;
    const ScanView *v = &g_scan_views[vi];
    UINT x, y, w, h, top;
    long holes = 0, runs = 0, through = 0;
    int longest = 0, bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    float s = F32(0x853EFF0u, 0);
    if (!ScanReadDepth(dev)) {
        static BOOL said;
        if (!said) { said = TRUE; Nx_Log("terrain scan: the depth buffer cannot be read here; no results"); }
        return;
    }
    w = g_scan_zw; h = g_scan_zh;
    top = h / 40 + 8;   /* the screen's top band: overlays */
    memset(g_scan_mask, 0, (size_t)w * h);
    for (x = 0; x < w; ++x) {
        float above = 3e38f;
        int run = 0;
        for (y = top; y < h; ++y) {
            float z = g_scan_z[(size_t)y * w + x];
            if (above < 1e38f && z > 1e38f) {
                g_scan_mask[(size_t)y * w + x] = 1;
                ++holes;
                if (!run) ++runs;
                ++run;
                if (run > longest) longest = run;
                bx0 = (int)x < bx0 ? (int)x : bx0; bx1 = (int)x > bx1 ? (int)x : bx1;
                by0 = (int)y < by0 ? (int)y : by0; by1 = (int)y > by1 ? (int)y : by1;
            } else {
                run = 0;
                if (above < 1e38f && z > above * 1.03f + 2.0f) { g_scan_mask[(size_t)y * w + x] = 2; ++through; }
            }
            if (z < above) above = z;
        }
    }
    g_scan_through += through;
    if (through >= 200) ++g_scan_through_views;
    if (holes < g_scan_min) return;
    ++g_scan_hit_views; g_scan_pixels += holes; ++g_scan_kind_hits[v->kind];
    Nx_Log("terrain scan: view %d/%d map %d (%s): %ld sky hole pixels in %ld runs (longest %d), box %d,%d-%d,%d; see-through %ld; nx_view: %.1f %.1f %.1f %.1f %.1f %.1f",
           vi + 1, g_scan_n, v->map, kScanKind[v->kind], holes, runs, longest, bx0, by0, bx1, by1, through,
           v->e[0] * s, v->e[1] * s, v->e[2] * s, v->a[0] * s, v->a[1] * s, v->a[2] * s);
    if (g_scan_written < g_scan_bmp && b && SUCCEEDED(Nx_ReadSurface(b)) && b->mem && b->fmt && b->width == w && b->height == h &&
        (b->fmt->bpp == 4 || b->fmt->bpp == 2)) {
        char path[MAX_PATH + 64];
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x)
                if (g_scan_mask[(size_t)y * w + x] == 1) {   /* sky holes magenta; see-through left as drawn */
                    BYTE *px = b->mem + (size_t)y * b->pitch + (size_t)x * b->fmt->bpp;
                    if (b->fmt->bpp == 4) { px[0] = 255; px[1] = 0; px[2] = 255; }
                    else { px[0] = 0x1F; px[1] = 0xF8; }
                }
        snprintf(path, sizeof path, "%s\\nx_scan", Renderer_GameFolder());
        CreateDirectoryA(path, NULL);
        snprintf(path, sizeof path, "%s\\nx_scan\\view%04d_map%d.bmp", Renderer_GameFolder(), vi + 1, v->map);
        if (Renderer_WriteBMP(path, b->width, b->height, b->mem, b->pitch, b->fmt->bpp)) ++g_scan_written;
    }
}

/* D_Present, before the frame is shown */
void Nx_TerrainScanFrame(NxDevice *dev)
{
    float s, e[3], a[3];
    const ScanView *v;
    if (!g_scan_on || g_scan_done || !g_scan_views) return;
    if (g_scan_i >= 0) {
        if (dev->frames - g_scan_set_frame < (unsigned)g_scan_hold) return;
        ScanAnalyze(dev, g_scan_i);
    }
    if (++g_scan_i >= g_scan_n) {
        int k;
        char kinds[256];
        size_t used = 0;
        kinds[0] = 0;
        for (k = 0; k < SK_COUNT; ++k)
            if (g_scan_kind_hits[k] && used < sizeof kinds - 40)
                used += (size_t)snprintf(kinds + used, sizeof kinds - used, "%s%s %ld", used ? ", " : "", kScanKind[k], g_scan_kind_hits[k]);
        Nx_Log("terrain scan: done, %d views, %d with sky holes (%.0f pixels)%s%s; see-through %.0f pixels, %d views with 200 or more",
               g_scan_n, g_scan_hit_views, g_scan_pixels, used ? ": " : "", kinds, g_scan_through, g_scan_through_views);
        g_scan_done = TRUE;
        if (g_scan_quit && dev->window) PostMessageA(dev->window, WM_CLOSE, 0, 0);
        return;
    }
    s = F32(0x853EFF0u, 0);
    if (s == 0) s = 1;
    v = &g_scan_views[g_scan_i];
    e[0] = v->e[0] * s; e[1] = v->e[1] * s; e[2] = v->e[2] * s;
    a[0] = v->a[0] * s; a[1] = v->a[1] * s; a[2] = v->a[2] * s;
    Nx_DevSetView(e, a);
    g_scan_set_frame = dev->frames;
}
