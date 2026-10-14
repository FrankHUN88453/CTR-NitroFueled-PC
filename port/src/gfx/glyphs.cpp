#include "glyphs.h"

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <filesystem>
#include <fstream>

#include "../hle/pad.h"
#include "../linker.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr u64 kAtlasHash = 0x6f5275ea19a00279ull;  // FNV-1a 64 of the PS4 atlas texels
constexpr u32 kW = kButtonAtlasWidth, kH = kButtonAtlasHeight;
constexpr size_t kAtlasBytes = (size_t)kW * kH * 4;

// atlas cells of the buttons (the font's alternate char metrics)
struct Cell {
    PadControl control;
    int x0, y0, x1, y1;
};
const Cell kCells[] = {
    {PadControl::L1, 0, 0, 146, 64},           {PadControl::R1, 147, 0, 293, 64},
    {PadControl::Cross, 294, 0, 378, 84},      {PadControl::Circle, 379, 0, 471, 84},
    {PadControl::Triangle, 0, 85, 92, 169},    {PadControl::Square, 93, 85, 185, 169},
    {PadControl::LeftStick, 271, 85, 355, 169}, {PadControl::RightStick, 356, 85, 440, 169},
    {PadControl::L2, 170, 170, 270, 262},      {PadControl::R2, 271, 170, 371, 262},
};

// PS4 atlas texels, premultiplied, in memory order B, G, R, A (the font's T# swizzles x <-> z)
std::vector<u8> g_original;

struct View {
    ComPtr<ID3D11ShaderResourceView> srv;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool made = false;
};
View g_xbox, g_keys[2];  // keyboard: driving, menus

u64 Fnv(const u8* p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Immutable texture with a box-filtered mip chain (premultiplied texels filter correctly).
ComPtr<ID3D11ShaderResourceView> MakeView(ID3D11Device* dev, DXGI_FORMAT fmt, const u8* rgba) {
    std::vector<std::vector<u8>> levels{{rgba, rgba + kAtlasBytes}};
    u32 w = kW, h = kH;
    while (w > 1 || h > 1) {
        u32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const std::vector<u8>& src = levels.back();
        std::vector<u8> dst((size_t)nw * nh * 4);
        for (u32 y = 0; y < nh; y++)
            for (u32 x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    u32 sum = 0;
                    for (u32 dy = 0; dy < 2; dy++)
                        for (u32 dx = 0; dx < 2; dx++) {
                            u32 sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
                            sum += src[((size_t)sy * w + sx) * 4 + c];
                        }
                    dst[((size_t)y * nw + x) * 4 + c] = (u8)((sum + 2) / 4);
                }
        levels.push_back(std::move(dst));
        w = nw, h = nh;
    }
    std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
    for (size_t m = 0; m < levels.size(); m++) init[m] = {levels[m].data(), std::max(1u, kW >> m) * 4, 0};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = (UINT)levels.size();
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(dev->CreateTexture2D(&td, init.data(), &tex)) ||
        FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return nullptr;
    return srv;
}

// ------------------------------------------------------------------ keyboard / mouse glyphs
namespace gp = Gdiplus;

const gp::Color kFace0(255, 72, 72, 72), kFace1(255, 16, 16, 16), kEdge(255, 232, 232, 232),
    kLabel(255, 255, 255, 255), kHighlight(255, 247, 162, 27);

void RoundRect(gp::GraphicsPath& p, gp::RectF r, float rad) {
    rad = std::min(rad, std::min(r.Width, r.Height) / 2);
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

std::wstring Wide(const char* utf8) {
    wchar_t w[32] = {};
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 31);
    return w;
}

// Bold label centred in `box`, as large as fits.
void Label(gp::Graphics& g, gp::RectF box, const std::wstring& text) {
    gp::FontFamily family(L"Arial");
    gp::GraphicsPath path;
    path.AddString(text.c_str(), -1, &family, gp::FontStyleBold, 100.0f, gp::PointF(0, 0),
                   gp::StringFormat::GenericTypographic());
    gp::RectF b;
    path.GetBounds(&b);
    if (b.Width <= 0 || b.Height <= 0) return;
    float s = std::min(box.Width / b.Width, box.Height / b.Height);
    gp::Matrix m;
    m.Translate(box.X + (box.Width - b.Width * s) / 2, box.Y + (box.Height - b.Height * s) / 2);
    m.Scale(s, s);
    m.Translate(-b.X, -b.Y);
    path.Transform(&m);
    gp::SolidBrush brush(kLabel);
    g.FillPath(&brush, &path);
}

void Keycap(gp::Graphics& g, gp::RectF r, const char* label) {
    gp::GraphicsPath path;
    RoundRect(path, r, r.Height * 0.22f);
    gp::LinearGradientBrush face(gp::PointF(0, r.Y), gp::PointF(0, r.GetBottom()), kFace0, kFace1);
    g.FillPath(&face, &path);
    gp::Pen edge(kEdge, std::max(2.0f, r.Height * 0.07f));
    g.DrawPath(&edge, &path);
    std::wstring text = Wide(label);
    // single characters fill more of the cap than words
    float fh = text.size() <= 1 ? 0.5f : text.size() <= 3 ? 0.36f : 0.3f;
    float pad = r.Height * 0.2f;
    Label(g, gp::RectF(r.X + pad, r.Y + r.Height * (1 - fh) / 2, r.Width - 2 * pad, r.Height * fh), text);
}

// Mouse with the given button highlighted (0 left, 1 right, 2 middle).
void Mouse(gp::Graphics& g, gp::RectF cell, int button) {
    float h = cell.Height * 0.9f, w = h * 0.64f;
    gp::RectF body(cell.X + (cell.Width - w) / 2, cell.Y + (cell.Height - h) / 2, w, h);
    gp::GraphicsPath path;
    RoundRect(path, body, w * 0.48f);
    gp::LinearGradientBrush face(gp::PointF(0, body.Y), gp::PointF(0, body.GetBottom()), kFace0, kFace1);
    g.FillPath(&face, &path);
    float split = body.Y + h * 0.42f, mid = body.X + w / 2;
    gp::SolidBrush hl(kHighlight);
    if (button < 2) {
        gp::Region clip(&path);
        g.SetClip(&clip);
        g.FillRectangle(&hl, gp::RectF(button == 0 ? body.X : mid, body.Y, w / 2, split - body.Y));
        g.ResetClip();
    }
    float lw = std::max(2.0f, h * 0.035f);
    gp::Pen line(kEdge, lw);
    g.DrawLine(&line, body.X, split, body.GetRight(), split);
    g.DrawLine(&line, mid, body.Y, mid, split);
    gp::RectF wheel(mid - w * 0.09f, body.Y + h * 0.1f, w * 0.18f, h * 0.2f);
    gp::GraphicsPath wp;
    RoundRect(wp, wheel, wheel.Width / 2);
    gp::SolidBrush dark(kFace1);
    g.FillPath(button == 2 ? (gp::Brush*)&hl : (gp::Brush*)&dark, &wp);
    g.DrawPath(&line, &wp);
    gp::Pen edge(kEdge, std::max(2.0f, h * 0.05f));
    g.DrawPath(&edge, &path);
}

// Up / left / down / right keys laid out like the arrow keys.
void KeyCluster(gp::Graphics& g, gp::RectF cell, PadControl stick, bool menu) {
    char buf[4][16];
    const char* labels[4];
    bool any = false;
    for (int d = 0; d < 4; d++) {
        int vk = PadPromptKey(stick, d, menu);
        labels[d] = vk ? PadKeyLabel(vk, buf[d], sizeof(buf[d])) : "";
        any |= vk != 0;
    }
    if (!any) return;
    float gap = 2, k = std::min((cell.Width - 2 * gap) / 3, (cell.Height - gap) / 2) - 1;
    float x0 = cell.X + (cell.Width - 3 * k - 2 * gap) / 2, y0 = cell.Y + (cell.Height - 2 * k - gap) / 2;
    Keycap(g, gp::RectF(x0 + k + gap, y0, k, k), labels[0]);
    Keycap(g, gp::RectF(x0, y0 + k + gap, k, k), labels[1]);
    Keycap(g, gp::RectF(x0 + k + gap, y0 + k + gap, k, k), labels[2]);
    Keycap(g, gp::RectF(x0 + 2 * (k + gap), y0 + k + gap, k, k), labels[3]);
}

std::vector<u8> DrawKeyboardAtlas(bool menu) {
    if (g_original.size() != kAtlasBytes) return {};
    std::vector<u8> atlas = g_original;
    ULONG_PTR token = 0;
    gp::GdiplusStartupInput input;
    if (gp::GdiplusStartup(&token, &input, nullptr) != gp::Ok) return {};
    for (const Cell& c : kCells) {
        int w = c.x1 - c.x0, h = c.y1 - c.y0;
        bool stick = c.control == PadControl::LeftStick || c.control == PadControl::RightStick;
        int vk = stick ? 0 : PadPromptKey(c.control, 0, menu);
        if (!stick && !vk) continue;  // unbound: keep the PS4 glyph
        gp::Bitmap bmp(w, h, PixelFormat32bppPARGB);
        {
            gp::Graphics g(&bmp);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
            g.Clear(gp::Color(0, 0, 0, 0));
            gp::RectF cell(0, 0, (float)w, (float)h);
            if (stick) {
                KeyCluster(g, cell, c.control, menu);
            } else if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
                Mouse(g, cell, vk == VK_LBUTTON ? 0 : vk == VK_RBUTTON ? 1 : 2);
            } else {
                char buf[16];
                const char* label = PadKeyLabel(vk, buf, sizeof(buf));
                float m = std::max(3.0f, h * 0.07f), kh = h - 2 * m;
                float kw = Wide(label).size() <= 2 ? kh : w - 2 * m;
                Keycap(g, gp::RectF((w - kw) / 2, m, kw, kh), label);
            }
        }
        gp::BitmapData bd;
        gp::Rect all(0, 0, w, h);
        if (bmp.LockBits(&all, gp::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != gp::Ok) continue;
        for (int y = 0; y < h; y++) {
            const u8* src = (const u8*)bd.Scan0 + (size_t)y * bd.Stride;
            u8* dst = atlas.data() + ((size_t)(c.y0 + y) * kW + c.x0) * 4;
            memcpy(dst, src, (size_t)w * 4);  // GDI+ PARGB is B, G, R, A in memory too
        }
        bmp.UnlockBits(&bd);
    }
    gp::GdiplusShutdown(token);
    return atlas;
}

std::vector<u8> DrawMenuAtlas() { return DrawKeyboardAtlas(true); }
std::vector<u8> DrawDrivingAtlas() { return DrawKeyboardAtlas(false); }

// embedded in the exe, or (release builds) glyphs_xbox.rgba in the game folder from tools/setup_shaders.py
std::vector<u8> LoadXboxAtlas() {
    HRSRC res = FindResourceA(nullptr, "GLYPHS_XBOX", MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
    const u8* p = h ? (const u8*)LockResource(h) : nullptr;
    if (p && SizeofResource(nullptr, res) == kAtlasBytes) return {p, p + kAtlasBytes};
    std::ifstream f(std::filesystem::path(g_linker.game_dir) / "glyphs_xbox.rgba", std::ios::binary);
    std::vector<u8> v(std::istreambuf_iterator<char>(f), {});
    return v.size() == kAtlasBytes ? v : std::vector<u8>{};
}

ID3D11ShaderResourceView* Get(View& v, ID3D11Device* dev, DXGI_FORMAT fmt, std::vector<u8> (*make)(), const char* what) {
    if (!v.made || v.fmt != fmt) {
        v.made = true;
        v.fmt = fmt;
        std::vector<u8> texels = make();
        v.srv = texels.size() == kAtlasBytes ? MakeView(dev, fmt, texels.data()) : nullptr;
        LOG("gfx: %s button glyphs %s", what, v.srv ? "ready" : "not available");
    }
    return v.srv.Get();
}

}  // namespace

bool IsButtonAtlas(u32 width, u32 height, const void* rgba) {
    return width == kW && height == kH && Fnv((const u8*)rgba, kAtlasBytes) == kAtlasHash;
}

void NoteButtonAtlas(const void* rgba) {
    if (g_original.empty()) g_original.assign((const u8*)rgba, (const u8*)rgba + kAtlasBytes);
}

ID3D11ShaderResourceView* ButtonAtlasOverride(ID3D11Device* dev, DXGI_FORMAT fmt) {
    switch (PadPromptStyle()) {
    case PromptStyle::Xbox: return Get(g_xbox, dev, fmt, LoadXboxAtlas, "Xbox");
    case PromptStyle::Keyboard: {
        const bool menu = PadMenuContext();
        return Get(g_keys[menu], dev, fmt, menu ? DrawMenuAtlas : DrawDrivingAtlas,
                   menu ? "keyboard/mouse (menus)" : "keyboard/mouse (driving)");
    }
    default: return nullptr;
    }
}
