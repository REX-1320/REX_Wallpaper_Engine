#include "WallpaperPage.h"
#include <sstream>

WallpaperPage::WallpaperPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                             const GlowStyle& style,
                             std::shared_ptr<rex::wallpaper::IWallpaperEngine> engine)
    : PageRenderer(rt, dwrite, style),
      m_engine(engine),
      m_inspector(rt, dwrite, style),
      m_libraryGallery(rt, dwrite, style, 6, 160, 120, 8)
{
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_bodyFormat);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.12f, 1.0f), &m_previewBgBrush);

    auto addBtn = [&](const std::wstring& label, std::function<void()> cb) {
        Button btn(label, rt, dwrite, style);
        btn.SetSize(36, 36);
        btn.SetOnClick(std::move(cb));
        m_transportButtons.push_back(std::move(btn));
    };
    addBtn(L"\u25C0", []{});
    addBtn(L"\u25B6", []{});
    addBtn(L"\u23F8", [this]() { if (m_engine) m_engine->Pause(); });
    addBtn(L"\u266A", []{});
    addBtn(L"\u26A1", []{});
    addBtn(L"\u2328", []{});
    addBtn(L"\u2665", []{});

    BuildCollections();
    LoadLibraryItems();
}

void WallpaperPage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    m_initialized = true;
}

void WallpaperPage::Update(float dt) {
    for (auto& btn : m_collectionButtons) btn.Update(dt);
    for (auto& btn : m_transportButtons) btn.Update(dt);
    m_libraryGallery.Update(dt);
}

void WallpaperPage::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;

    float collX = m_x, collY = m_y + 8, collW = 200, collH = m_h - 8;

    float previewX = collX + collW + 8;
    float previewW = m_w - collW - 288;
    if (previewW < 100) previewW = 100;
    float previewY = m_y + 8;
    float previewH = m_h - 160;

    float inspX = previewX + previewW + 8;
    float inspW = 270;

    auto previewRect = D2D1::RectF(previewX, previewY, previewX + previewW, previewY + previewH);
    rt->FillRectangle(previewRect, m_previewBgBrush.Get());

    std::wstring placeholder = L"Live Preview";
    rt->DrawTextW(placeholder.c_str(), (UINT32)placeholder.size(), m_bodyFormat.Get(),
        previewRect, m_textBrush.Get());

    float transportY = previewY + previewH + 6;
    float btnX = previewX + 10;
    for (auto& btn : m_transportButtons) {
        btn.SetPosition(btnX, transportY);
        btn.Render(rt);
        btnX += 42;
    }

    float y = collY;
    for (size_t i = 0; i < m_collectionButtons.size(); ++i) {
        m_collectionButtons[i].SetPosition(collX + 8, y);
        m_collectionButtons[i].SetSelected(i == m_activeCollection);
        m_collectionButtons[i].Render(rt);
        y += 28;
    }

    m_inspector.SetBounds(inspX, previewY, inspW);
    m_inspector.Render(rt);

    float libY = transportY + 44;
    float libH = m_h - libY - 4;
    m_libraryGallery.SetBounds(collX + collW, libY, m_w - collW - inspW, libH);
    m_libraryGallery.SetItems(m_libraryItems);
    m_libraryGallery.Render(rt);
}

void WallpaperPage::HandleClick(float mx, float my) {
    for (size_t i = 0; i < m_collectionButtons.size(); ++i) {
        if (m_collectionButtons[i].HitTest(mx, my)) {
            OnCollectionSelected((int)i);
            return;
        }
    }
    for (auto& btn : m_transportButtons) {
        if (btn.HitTest(mx, my)) { auto cb = btn.GetOnClick(); if (cb) cb(); return; }
    }
    m_libraryGallery.HitTest(mx, my);
}

void WallpaperPage::ScanDirectory(const std::wstring& path) {
    (void)path;
    LoadLibraryItems();
}

void WallpaperPage::BuildCollections() {
    m_collections = {
        L"   * Favorites", L"   ~ Recent", L"   > Videos",
        L"   # Images", L"   @ Web", L"   ^ Shader",
        L"   & Scene", L"   + Audio",
        L"   [] Local", L"   () Online", L"   Playlists",
        L"   Downloads", L"   Trash"
    };

    for (size_t i = 0; i < m_collections.size(); ++i) {
        GlowTextButton btn(m_collections[i], m_rt, m_dwrite, m_style);
        btn.SetFontSize(14.0f);
        btn.SetOnClick([this, i]() { OnCollectionSelected((int)i); });
        m_collectionButtons.push_back(std::move(btn));
    }
}

void WallpaperPage::OnCollectionSelected(int index) {
    m_activeCollection = index;
    LoadLibraryItems();
}

void WallpaperPage::LoadLibraryItems() {
    m_libraryItems.clear();
    for (int i = 0; i < 30; ++i) {
        GalleryItem item;
        item.title = L"Wallpaper " + std::to_wstring(i);
        item.subtitle = L"1920\u00d71080";
        m_libraryItems.push_back(item);
    }
    m_libraryGallery.SetItems(m_libraryItems);
    m_libraryGallery.SetOnItemClick([this](int idx, const GalleryItem&) {
        OnWallpaperSelected(idx);
    });
}

void WallpaperPage::OnWallpaperSelected(int index) {
    UpdateInspector(index);
    if (m_engine && index >= 0 && (size_t)index < m_libraryItems.size()) {
        rex::wallpaper::SceneInfo si;
        si.id = std::to_string(index);
        si.name = std::string(m_libraryItems[index].title.begin(), m_libraryItems[index].title.end());
        si.sourcePath = si.name + ".mp4";
        si.type = "video";
        m_engine->LoadScene(si);
    }
}

void WallpaperPage::UpdateInspector(int index) {
    std::vector<PropertyRow> props;
    if (index >= 0 && (size_t)index < m_libraryItems.size()) {
        props.push_back({L"Name", m_libraryItems[index].title});
        props.push_back({L"Type", L"Video"});
        props.push_back({L"Resolution", L"3840\u00d72160"});
    } else {
        props.push_back({L"Name", L"None"});
    }
    props.push_back({L"Renderer", L"Video"});
    props.push_back({L"FPS", L"60"});
    props.push_back({L"GPU", L"5%"});
    props.push_back({L"Tags", L""});
    props.push_back({L"Playlist", L""});
    m_inspector.SetProperties(props);
}
