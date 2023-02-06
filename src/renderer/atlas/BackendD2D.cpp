#include "pch.h"
#include "BackendD2D.h"

using namespace Microsoft::Console::Render::Atlas;

ID2D1Brush* BackendD2D::_brushWithColor(u32 color)
{
    if (_brushColor != color)
    {
        const auto d2dColor = colorFromU32(color);
        THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&d2dColor, nullptr, _brush.put()));
        _brushColor = color;
    }
    return _brush.get();
}

BackendD2D::BackendD2D(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext) :
    _device{ std::move(device) },
    _deviceContext{ std::move(deviceContext) }
{
}

void BackendD2D::Render(const RenderingPayload& p)
{
    _swapChainManager.UpdateSwapChainSettings(
        p,
        _device.get(),
        [this]() {
            _d2dRenderTarget.reset();
            _deviceContext->ClearState();
        },
        [this]() {
            _d2dRenderTarget.reset();
            _deviceContext->ClearState();
            _deviceContext->Flush();
        });

    if (_fontGeneration != p.s->font.generation())
    {
        {
            wil::com_ptr<ID3D11Texture2D> buffer;
            THROW_IF_FAILED(_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));

            const auto surface = buffer.query<IDXGISurface>();

            D2D1_RENDER_TARGET_PROPERTIES props{};
            props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.dpiX = static_cast<float>(p.s->font->dpi);
            props.dpiY = static_cast<float>(p.s->font->dpi);
            wil::com_ptr<ID2D1RenderTarget> renderTarget;
            THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
            _d2dRenderTarget = renderTarget.query<ID2D1DeviceContext>();
            _d2dRenderTarget4 = renderTarget.query<ID2D1DeviceContext4>();
            _d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(p.s->misc->antialiasingMode));
        }
        {
            static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
            THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, _brush.put()));
            _brushColor = 0xffffffff;
        }
        {
            D2D1_BITMAP_PROPERTIES props{};
            props.pixelFormat = { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.dpiX = static_cast<float>(p.s->font->dpi);
            props.dpiY = static_cast<float>(p.s->font->dpi);
            THROW_IF_FAILED(_d2dRenderTarget->CreateBitmap({ p.s->cellCount.x, p.s->cellCount.y }, props, _d2dBackgroundBitmap.put()));
            THROW_IF_FAILED(_d2dRenderTarget->CreateBitmapBrush(_d2dBackgroundBitmap.get(), _d2dBackgroundBrush.put()));
            _d2dBackgroundBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            _d2dBackgroundBrush->SetTransform(D2D1::Matrix3x2F::Scale(p.s->font->cellSize.x, p.s->font->cellSize.y));
        }
        _fontGeneration = p.s->font.generation();
    }
    _d2dRenderTarget->BeginDraw();

    _d2dBackgroundBitmap->CopyFromMemory(nullptr, p.backgroundBitmap.data(), p.s->cellCount.x * 4);
    _d2dRenderTarget->FillRectangle({ 0, 0, p.s->cellCount.x * p.d.font.cellSizeDIP.x, p.s->cellCount.y * p.d.font.cellSizeDIP.y }, _d2dBackgroundBrush.get());

    size_t y = 0;
    for (const auto& row : p.rows)
    {
        for (const auto& m : row.mappings)
        {
            DWRITE_GLYPH_RUN glyphRun{};
            glyphRun.fontFace = m.fontFace.get();
            glyphRun.fontEmSize = m.fontEmSize;
            glyphRun.glyphCount = m.glyphsTo - m.glyphsFrom;
            glyphRun.glyphIndices = &row.glyphIndices[m.glyphsFrom];
            glyphRun.glyphAdvances = &row.glyphAdvances[m.glyphsFrom];
            glyphRun.glyphOffsets = &row.glyphOffsets[m.glyphsFrom];

            const D2D1_POINT_2F baseline{
                0, // TODO
                p.d.font.cellSizeDIP.y * y + p.s->font->baselineInDIP,
            };

            _drawGlyphRun(baseline, &glyphRun, _brush.get());
        }

        y++;
    }

    THROW_IF_FAILED(_d2dRenderTarget->EndDraw());

    THROW_IF_FAILED(_d2dRenderTarget->EndDraw());
}

void BackendD2D::WaitUntilCanRender() noexcept
{
}

// See _drawGlyph() for reference.
u16 BackendD2D::_d2dDrawGlyph(const RenderingPayload& p, const TileHashMap::iterator& it, const u16x2 coord, const u32 color)
{
    const auto key = it->first.data();
    const auto value = it->second.data();
    const auto charsLength = key->charCount;
    const auto cellCount = key->attributes.cellCount;
    const auto textFormat = p.d.font.textFormats[key->attributes.italic][key->attributes.bold].get();
    const auto coloredGlyph = WI_IsFlagSet(value->flags, CellFlags::ColoredGlyph);

    auto& cachedLayout = it->second.cachedLayout;
    if (!cachedLayout)
    {
        cachedLayout = _getCachedGlyphLayout(p, &key->chars[0], charsLength, cellCount, textFormat, coloredGlyph);
    }

    D2D1_RECT_F rect;
    rect.left = static_cast<float>(coord.x) * p.d.font.cellSizeDIP.x;
    rect.top = static_cast<float>(coord.y) * p.d.font.cellSizeDIP.y;
    rect.right = static_cast<float>(coord.x + cellCount) * p.d.font.cellSizeDIP.x;
    rect.bottom = rect.top + p.d.font.cellSizeDIP.y;

    D2D1_POINT_2F origin;
    origin.x = rect.left;
    origin.y = rect.top;

    _d2dRenderTarget->PushAxisAlignedClip(&rect, D2D1_ANTIALIAS_MODE_ALIASED);

    cachedLayout.applyScaling(_d2dRenderTarget.get(), origin);

    origin.x += cachedLayout.offset.x;
    origin.y += cachedLayout.offset.y;
    _d2dRenderTarget->DrawTextLayout(origin, cachedLayout.textLayout.get(), _brushWithColor(color), cachedLayout.options);

    cachedLayout.undoScaling(_d2dRenderTarget.get());

    _d2dRenderTarget->PopAxisAlignedClip();

    return cellCount;
}

void BackendD2D::_d2dDrawLine(const RenderingPayload& p, u16r rect, u16 pos, u16 width, u32 color, ID2D1StrokeStyle* strokeStyle)
{
    const auto w = static_cast<float>(width) * p.d.font.dipPerPixel;
    const auto y1 = static_cast<float>(rect.top) * p.d.font.cellSizeDIP.y + static_cast<float>(pos) * p.d.font.dipPerPixel + w * 0.5f;
    const auto x1 = static_cast<float>(rect.left) * p.d.font.cellSizeDIP.x;
    const auto x2 = static_cast<float>(rect.right) * p.d.font.cellSizeDIP.x;
    const auto brush = _brushWithColor(color);
    _d2dRenderTarget->DrawLine({ x1, y1 }, { x2, y1 }, brush, w, strokeStyle);
}

void BackendD2D::_d2dFillRectangle(const RenderingPayload& p, u16r rect, u32 color)
{
    const D2D1_RECT_F r{
        .left = static_cast<float>(rect.left) * p.d.font.cellSizeDIP.x,
        .top = static_cast<float>(rect.top) * p.d.font.cellSizeDIP.y,
        .right = static_cast<float>(rect.right) * p.d.font.cellSizeDIP.x,
        .bottom = static_cast<float>(rect.bottom) * p.d.font.cellSizeDIP.y,
    };
    const auto brush = _brushWithColor(color);
    _d2dRenderTarget->FillRectangle(r, brush);
}

void BackendD2D::_d2dCellFlagRendererCursor(const RenderingPayload& p, u16r rect, u32 color)
{
    _drawCursor(p, _d2dRenderTarget.get(), rect, _brushWithColor(p.s->cursor->cursorColor), false);
}

void BackendD2D::_d2dCellFlagRendererSelected(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dFillRectangle(p, rect, p.s->misc->selectionColor);
}

void BackendD2D::_d2dCellFlagRendererUnderline(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dDrawLine(p, rect, p.s->font->underlinePos, p.s->font->underlineWidth, color);
}

void BackendD2D::_d2dCellFlagRendererUnderlineDotted(const RenderingPayload& p, u16r rect, u32 color)
{
    if (!_dottedStrokeStyle)
    {
        static constexpr D2D1_STROKE_STYLE_PROPERTIES props{ .dashStyle = D2D1_DASH_STYLE_CUSTOM };
        static constexpr FLOAT dashes[2]{ 1, 2 };
        THROW_IF_FAILED(p.d2dFactory->CreateStrokeStyle(&props, &dashes[0], 2, _dottedStrokeStyle.addressof()));
    }

    _d2dDrawLine(p, rect, p.s->font->underlinePos, p.s->font->underlineWidth, color, _dottedStrokeStyle.get());
}

void BackendD2D::_d2dCellFlagRendererUnderlineDouble(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dDrawLine(p, rect, p.s->font->doubleUnderlinePos.x, p.s->font->thinLineWidth, color);
    _d2dDrawLine(p, rect, p.s->font->doubleUnderlinePos.y, p.s->font->thinLineWidth, color);
}

void BackendD2D::_d2dCellFlagRendererStrikethrough(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dDrawLine(p, rect, p.s->font->strikethroughPos, p.s->font->strikethroughWidth, color);
}
