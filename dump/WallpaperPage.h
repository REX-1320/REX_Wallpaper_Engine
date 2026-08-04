#pragma once
#include "PageRenderer.h"
#include "Gallery.h"
#include "PropertyGrid.h"
#include "Button.h"
#include "GlowStyle.h"
#include "rex/wallpaper/IWallpaperEngine.hpp"
#include <memory>
#include <vector>

class WallpaperPage : public PageRenderer {
public:
    WallpaperPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                  const GlowStyle& style,
                  std::shared_ptr<rex::wallpaper::IWallpaperEngine> engine);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt) override;
    void Render(ID2D1RenderTarget* rt) override;
    void HandleClick(float mx, float my) override;
    void ScanDirectory(const std::wstring& path);

private:
    std::shared_ptr<rex::wallpaper::IWallpaperEngine> m_engine;

    std::vector<std::wstring> m_collections;
    std::vector<GlowTextButton> m_collectionButtons;
    int m_activeCollection = 2;

    float m_previewWidth = 0, m_previewHeight = 0;
    std::vector<Button> m_transportButtons;

    PropertyGrid m_inspector;
    Gallery m_libraryGallery;
    std::vector<GalleryItem> m_libraryItems;

    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
    bool m_initialized = false;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_previewBgBrush;

    void BuildCollections();
    void LoadLibraryItems();
    void OnCollectionSelected(int index);
    void OnWallpaperSelected(int libraryIndex);
    void UpdateInspector(int index);
};
