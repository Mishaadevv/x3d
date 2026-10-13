// Tables (resizable/sortable columns, row backgrounds) and the list clipper (virtualized lists).

#include "../Core/Internal.h"

namespace zx::ui {

namespace {

constexpr f32 MinColumnWidth = 30.0f;

void ResolveColumns(Context::Impl& g, TableState& t)
{
    const f32 width = t.rect.Width();
    const f32 minW = g.S(MinColumnWidth);
    if (t.savedScale != g.scale) {
        for (f32& w : t.savedWidths)
            w *= g.scale / t.savedScale;
        t.savedScale = g.scale;
    }
    f32 fixed = 0.0f;
    u32 stretch = 0;
    for (u32 i = 0; i < t.columnCount; ++i) {
        TableColumn& c = t.columns[i];
        const f32 saved = t.savedWidths[i];
        c.width = saved > 0.0f ? saved : (c.initWidth > 0.0f ? g.S(c.initWidth) : 0.0f);
        if (c.width > 0.0f)
            fixed += Max(c.width, minW);
        else
            ++stretch;
    }
    const f32 remaining = Max(width - fixed, minW * f32(stretch));
    f32 x = t.rect.min.x;
    for (u32 i = 0; i < t.columnCount; ++i) {
        TableColumn& c = t.columns[i];
        c.width = c.width > 0.0f ? Max(c.width, minW) : Round(remaining / f32(Max(stretch, 1u)));
        // The last column absorbs rounding so the table width is exact.
        if (i + 1 == t.columnCount)
            c.width = Max(t.rect.max.x - x, minW);
        c.x = Round(x);
        x += c.width;
    }
    t.setupDone = true;
}

void FinishCell(Context::Impl& g, TableState& t)
{
    if (t.column < 0)
        return;
    g.current->dl->PopClipRect();
    t.column = -1;
}

void FinishRow(Context::Impl& g, TableState& t)
{
    if (t.rowIndex < 0 || !t.rowOpen)
        return;
    FinishCell(g, t);
    WindowState* w = g.current;
    t.rowHeight = Max(t.rowHeight, Round(w->cursorMax.y + t.cellPadY - t.rowY));
    const Rect row(Vec2(t.rect.min.x, t.rowY), Vec2(t.rect.max.x, t.rowY + t.rowHeight));
    const Theme& c = g.style.colors;
    Color bg(0, 0, 0, 0);
    if (t.headerRow)
        bg = c.tableHeaderBg;
    else if (HasAnyFlags(t.flags, TableFlags::RowBg) && (t.rowIndex % 2) == 1)
        bg = c.tableRowAlt;
    if (!t.headerRow && w->contentHoverable && g.IsHoveringRect(row) && g.activeId == 0)
        bg = LerpColor(bg.a ? bg : Color(c.headerHovered.r, c.headerHovered.g, c.headerHovered.b, 0), c.headerHovered, 0.6f);
    if (bg.a)
        w->dl->InsertRectAt(t.rowBgInsertIndex, row, g.Col(bg));
    if (HasAnyFlags(t.flags, TableFlags::Borders))
        w->dl->AddRect(Rect(Vec2(row.min.x, row.max.y - 1.0f), row.max), g.Col(t.headerRow ? c.borderStrong : c.tableBorder));
    t.rowY += t.rowHeight;
    t.rowOpen = false;
    t.headerRow = false;
}

void BeginCell(Context::Impl& g, TableState& t, u32 column)
{
    WindowState* w = g.current;
    t.column = i32(column);
    const TableColumn& c = t.columns[column];
    w->workMinX = c.x + t.cellPadX;
    w->workMaxX = c.x + c.width - t.cellPadX;
    w->indent = 0.0f;
    w->cursor = Vec2(w->workMinX, t.rowY + t.cellPadY);
    w->currLineHeight = 0.0f;
    w->prevLineEnd = w->cursor;
    w->dl->PushClipRect(Rect(Vec2(c.x, t.rowY), Vec2(c.x + c.width, Max(w->clip.max.y, t.rowY + 1.0f))));
}

} // namespace

bool BeginTable(StringView idStr, u32 columns, TableFlags flags, Vec2 outerSize)
{
    Context::Impl& g = G();
    ZX_ASSERT(columns > 0 && columns < 64, "BeginTable: 1..63 columns");
    const ID id = g.MakeID(idStr);
    bool scroll = false;
    if (HasAnyFlags(flags, TableFlags::ScrollY) && outerSize.y > 0.0f) {
        if (!BeginChild(String::Format("{}##tablescroll", idStr), outerSize, true)) {
            EndChild();
            return false;
        }
        scroll = true;
    }
    WindowState* w = g.current;
    UniquePtr<TableState>* found = g.tables.Find(id);
    if (!found)
        found = g.tables.Insert(ID(id), UniquePtr<TableState>(new TableState())).first;
    TableState& t = **found;
    t.id = id;
    t.flags = flags;
    if (t.columnCount != columns) {
        t.columnCount = columns;
        t.savedWidths.Resize(columns);
        for (f32& sw : t.savedWidths)
            sw = 0.0f;
        t.savedScale = g.scale;
    }
    t.columns.Resize(columns);
    for (TableColumn& c : t.columns) {
        c.label.Clear();
        c.initWidth = 0.0f;
    }
    t.setupIndex = 0;
    t.hasScroll = scroll;
    t.setupDone = false;
    t.sort.dirty = false;
    const f32 width = !scroll && outerSize.x > 0.0f ? g.S(outerSize.x) : w->workMaxX - w->cursor.x;
    t.rect = Rect(w->cursor, Vec2(w->cursor.x + Round(width), w->cursor.y));
    t.rowY = w->cursor.y;
    t.rowIndex = -1;
    t.rowOpen = false;
    t.column = -1;
    t.cellPadX = Round(g.S(6.0f));
    t.cellPadY = Round(g.S(3.0f));
    t.savedWorkMinX = w->workMinX;
    t.savedWorkMaxX = w->workMaxX;
    t.savedIndent = w->indent;
    t.savedCursorMax = w->cursorMax;
    g.tableStack.PushBack(&t);
    w->idStack.PushBack(id);
    return true;
}

void TableSetupColumn(StringView label, f32 width)
{
    Context::Impl& g = G();
    TableState& t = *g.tableStack.Back();
    if (t.setupIndex >= t.columnCount)
        return;
    TableColumn& c = t.columns[t.setupIndex++];
    c.label = label;
    c.initWidth = width;
}

void TableNextRow(f32 minHeight)
{
    Context::Impl& g = G();
    TableState& t = *g.tableStack.Back();
    if (!t.setupDone)
        ResolveColumns(g, t);
    FinishRow(g, t);
    ++t.rowIndex;
    t.rowOpen = true;
    t.rowHeight = Max(Round(g.S(minHeight)), g.LineHeight() + t.cellPadY * 2.0f);
    t.rowBgInsertIndex = g.current->dl->PrimitiveCount();
    g.current->cursorMax.y = t.rowY; // per-row content height measurement
}

bool TableSetColumnIndex(u32 column)
{
    Context::Impl& g = G();
    TableState& t = *g.tableStack.Back();
    if (!t.rowOpen)
        TableNextRow();
    if (column >= t.columnCount)
        return false;
    FinishCell(g, t);
    BeginCell(g, t, column);
    return true;
}

bool TableNextColumn()
{
    Context::Impl& g = G();
    TableState& t = *g.tableStack.Back();
    if (!t.rowOpen) {
        TableNextRow();
        BeginCell(g, t, 0);
        return true;
    }
    const u32 next = u32(t.column + 1);
    FinishCell(g, t);
    if (next >= t.columnCount) {
        TableNextRow();
        BeginCell(g, t, 0);
        return true;
    }
    BeginCell(g, t, next);
    return true;
}

void TableHeadersRow()
{
    Context::Impl& g = G();
    TableState& t = *g.tableStack.Back();
    TableNextRow();
    t.headerRow = true;
    const Theme& col = g.style.colors;
    PushFont(FontStyle::SemiBold);
    for (u32 i = 0; i < t.columnCount; ++i) {
        TableSetColumnIndex(i);
        WindowState* w = g.current;
        const TableColumn& c = t.columns[i];
        const Rect cell(Vec2(c.x, t.rowY), Vec2(c.x + c.width, t.rowY + g.LineHeight() + t.cellPadY * 2.0f));
        const ID hid = g.MakeID(i32(1000 + i));
        g.ItemAdd(cell, hid);
        bool hovered = false, held = false;
        const bool sortable = HasAnyFlags(t.flags, TableFlags::Sortable);
        // Leave the right edge to the column resizer.
        const Rect clickRect(cell.min, cell.max - Vec2(g.S(4.0f), 0.0f));
        if (sortable && g.ButtonBehavior(clickRect, hid, &hovered, &held)) {
            if (t.sort.column == i32(i))
                t.sort.ascending = !t.sort.ascending;
            else {
                t.sort.column = i32(i);
                t.sort.ascending = true;
            }
            t.sort.dirty = true;
        }
        if (hovered)
            w->dl->AddRect(cell, g.Col(col.headerHovered));
        f32 textMaxX = cell.max.x - t.cellPadX;
        if (sortable && t.sort.column == i32(i)) {
            const f32 iw = Round(g.S(14.0f));
            g.PushFontSmall();
            g.RenderTextClipped(Rect(Vec2(textMaxX - iw, cell.min.y), Vec2(textMaxX, cell.max.y)),
                                t.sort.ascending ? ZX_ICON_CHEVRON_UP : ZX_ICON_CHEVRON_DOWN, g.Col(col.accent), TextAlign::Center);
            g.fontStack.PopBack();
            textMaxX -= iw + g.S(2.0f);
        }
        g.RenderTextClipped(Rect(Vec2(cell.min.x + t.cellPadX, cell.min.y), Vec2(textMaxX, cell.max.y)), c.label, g.TextColor());
        g.ItemSize(Vec2(0.0f, cell.Height() - t.cellPadY * 2.0f));
    }
    PopFont();
}

TableSortSpec TableGetSortSpec()
{
    Context::Impl& g = G();
    return g.tableStack.Back()->sort;
}

void EndTable()
{
    Context::Impl& g = G();
    ZX_ASSERT(!g.tableStack.Empty(), "EndTable without BeginTable");
    TableState& t = *g.tableStack.Back();
    WindowState* w = g.current;
    if (!t.setupDone)
        ResolveColumns(g, t);
    FinishRow(g, t);
    t.rect.max.y = t.rowY;
    const Theme& c = g.style.colors;
    // Column borders + resize handles.
    for (u32 i = 0; i + 1 < t.columnCount; ++i) {
        const f32 x = t.columns[i].x + t.columns[i].width;
        if (HasAnyFlags(t.flags, TableFlags::Borders))
            w->dl->AddRect(Rect(Vec2(x - 1.0f, t.rect.min.y), Vec2(x, t.rect.max.y)), g.Col(c.tableBorder));
        if (HasAnyFlags(t.flags, TableFlags::Resizable)) {
            const ID rid = g.MakeID(i32(5000 + i));
            const Rect handle(Vec2(x - g.S(4.0f), t.rect.min.y), Vec2(x + g.S(3.0f), t.rect.max.y));
            g.ItemAdd(handle, rid);
            bool hovered, held;
            g.ButtonBehavior(handle, rid, &hovered, &held);
            if (hovered || held) {
                g.SetCursorShape(CursorShape::ResizeEW);
                w->dl->AddRect(Rect(Vec2(x - 1.0f, t.rect.min.y), Vec2(x + 1.0f, t.rect.max.y)), c.accent);
            }
            if (held && g.mouseDelta.x != 0.0f) {
                // Freeze all columns to their current widths, then move the border between i and i+1.
                const f32 minW = g.S(MinColumnWidth);
                for (u32 k = 0; k + 1 < t.columnCount; ++k)
                    t.savedWidths[k] = t.columns[k].width;
                f32 d = g.mouseDelta.x;
                d = Clamp(d, minW - t.columns[i].width, i + 2 < t.columnCount ? t.columns[i + 1].width - minW : F32Max);
                t.savedWidths[i] += d;
                if (i + 2 < t.columnCount)
                    t.savedWidths[i + 1] -= d;
                t.savedScale = g.scale;
            }
        }
    }
    if (HasAnyFlags(t.flags, TableFlags::Borders))
        w->dl->AddRectOutline(t.rect, g.Col(c.tableBorder), 1.0f);
    // Restore the window layout and advance past the table.
    w->workMinX = t.savedWorkMinX;
    w->workMaxX = t.savedWorkMaxX;
    w->indent = t.savedIndent;
    w->cursorMax = Max(t.savedCursorMax, Vec2(t.rect.max.x, t.rect.min.y));
    w->cursor = t.rect.min;
    w->currLineHeight = 0.0f;
    g.ItemSize(t.rect.Size());
    g.ItemAdd(t.rect, t.id);
    w->idStack.PopBack();
    g.tableStack.PopBack();
    if (t.hasScroll)
        EndChild();
}

// --- List clipper ----------------------------------------------------------------------------------------------------------

ListClipper::ListClipper(u32 itemCount, f32 itemHeight) : m_count(itemCount), m_itemHeight(itemHeight)
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    if (!g.tableStack.Empty()) {
        // Table rows: fixed row height, no item spacing.
        TableState& t = *g.tableStack.Back();
        if (!t.setupDone)
            ResolveColumns(g, t);
        FinishRow(g, t);
        m_startY = t.rowY;
        if (m_itemHeight <= 0.0f)
            m_itemHeight = g.LineHeight() + t.cellPadY * 2.0f;
        m_step = m_itemHeight;
    } else {
        m_itemHeight = Max(m_itemHeight, 1.0f);
        m_startY = w->cursor.y;
        m_step = m_itemHeight + g.S(g.style.itemSpacing.y);
    }
    const f32 first = Floor((w->clip.min.y - m_startY) / m_step);
    const f32 last = Ceil((w->clip.max.y - m_startY) / m_step);
    begin = u32(Clamp(first, 0.0f, f32(m_count)));
    end = u32(Clamp(last, f32(begin), f32(m_count)));
    if (!g.tableStack.Empty()) {
        TableState& t = *g.tableStack.Back();
        t.rowY = m_startY + f32(begin) * m_step;
        t.rowIndex += i32(begin);
    } else if (begin > 0) {
        w->cursor.y = Round(m_startY + f32(begin) * m_step);
    }
}

void ListClipper::End()
{
    Context::Impl& g = G();
    WindowState* w = g.current;
    const f32 endY = m_startY + f32(m_count) * m_step;
    if (!g.tableStack.Empty()) {
        TableState& t = *g.tableStack.Back();
        FinishRow(g, t);
        t.rowIndex += i32(m_count - end);
        t.rowY = Max(t.rowY, endY);
    } else {
        w->cursor.y = Max(w->cursor.y, Round(endY));
        w->cursorMax.y = Max(w->cursorMax.y, endY - g.S(g.style.itemSpacing.y));
    }
}

} // namespace zx::ui
