#include "Table.h"

Table::Table(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_headerFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_cellFormat);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,0.05f), &m_headerBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,0.02f), &m_rowBgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.4f,0.4f,0.4f,0.5f), &m_borderBrush);
}

void Table::SetBounds(float x, float y, float width, float height) {
    m_x = x; m_y = y; m_width = width; m_height = height;
}

void Table::SetColumns(const std::vector<TableColumn>& cols) { m_columns = cols; }
void Table::SetRows(const std::vector<TableRow>& rows) { m_rows = rows; }

bool Table::HitTest(float mx, float my) {
    return mx >= m_x && mx <= m_x + m_width && my >= m_y && my <= m_y + m_height;
}

void Table::Update(float dt) {}

void Table::Render(ID2D1RenderTarget* rt) {
    float y = m_y;
    rt->FillRectangle(D2D1::RectF(m_x, y, m_x + m_width, y + m_headerHeight), m_headerBgBrush.Get());
    float x = m_x;
    for (auto& col : m_columns) {
        rt->DrawTextW(col.header.c_str(), (UINT32)col.header.size(), m_headerFormat.Get(),
            D2D1::RectF(x, y, x + col.width, y + m_headerHeight), m_textBrush.Get());
        x += col.width;
    }
    y += m_headerHeight;

    for (size_t i = 0; i < m_rows.size(); ++i) {
        auto& row = m_rows[i];
        float rowY = y + i * m_rowHeight;
        if (rowY + m_rowHeight < m_y || rowY > m_y + m_height) continue;
        if (row.selected || (int)i == m_hoveredRow) {
            rt->FillRectangle(D2D1::RectF(m_x, rowY, m_x + m_width, rowY + m_rowHeight),
                m_rowBgBrush.Get());
        }
        x = m_x;
        for (size_t j = 0; j < std::min(row.cells.size(), m_columns.size()); ++j) {
            rt->DrawTextW(row.cells[j].c_str(), (UINT32)row.cells[j].size(), m_cellFormat.Get(),
                D2D1::RectF(x, rowY, x + m_columns[j].width, rowY + m_rowHeight), m_textBrush.Get());
            x += m_columns[j].width;
        }
    }
}
