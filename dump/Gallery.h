#pragma once
#include <vector>
#include <functional>
#include <any>
#include <cmath>
#include <d2d1_3.h>
#include "Card.h"
#include "SpringAnimation.h"

struct GalleryItem {
    std::wstring title;
    std::wstring subtitle;
    std::wstring imagePath;
    std::any userData;
};

class Gallery {
public:
    Gallery(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style,
            int columns = 3, float cardWidth = 260, float cardHeight = 200, float gap = 16);

    void SetBounds(float x, float y, float width, float height);
    void SetItems(const std::vector<GalleryItem>& items);
    void SetOnItemClick(std::function<void(int index, const GalleryItem&)> cb) { m_onItemClick = std::move(cb); }

    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

    void ScrollBy(float deltaY);
    void ScrollTo(float offsetY);

private:
    float m_x, m_y, m_width, m_height;
    int m_columns;
    float m_cardWidth, m_cardHeight, m_gap;
    float m_scrollOffset = 0;
    float m_maxScroll = 0;
    SpringAnimation m_scrollSpring{200,20,1,0};

    std::vector<GalleryItem> m_items;
    std::vector<Card> m_cards;
    std::function<void(int, const GalleryItem&)> m_onItemClick;

    GlowStyle m_style;
    ID2D1RenderTarget* m_rt;
    IDWriteFactory* m_dwrite;

    int m_hoveredIndex = -1;

    void RebuildCards();
};
