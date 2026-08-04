#pragma once
#include <vector>
#include <string>
#include <functional>
#include <any>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GlowStyle.h"

struct TableColumn {
    std::wstring header;
    float width;
};

struct TableRow {
    std::vector<std::wstring> cells;
    std::any userData;
    bool selected = false;
};

class Table {
public:
    Table(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);

    void SetBounds(float x, float y, float width, float height);
    void SetColumns(const std::vector<TableColumn>& cols);
    void SetRows(const std::vector<TableRow>& rows);
    void SetOnRowClick(std::function<void(int index, const TableRow&)> cb) { m_onRowClick = std::move(cb); }

    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width, m_height;
    float m_rowHeight = 28;
    float m_headerHeight = 32;
    std::vector<TableColumn> m_columns;
    std::vector<TableRow> m_rows;
    int m_hoveredRow = -1;
    int m_sortedColumn = -1;
    bool m_sortAscending = true;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_headerFormat, m_cellFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush, m_headerBgBrush, m_rowBgBrush, m_borderBrush;
    GlowStyle m_style;
    std::function<void(int, const TableRow&)> m_onRowClick;
};
