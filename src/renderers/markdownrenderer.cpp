#include "markdownrenderer.hpp"

#include "textstyleinstance.hpp"

#include "renderhelpers.hpp"

#include <cmark.h>
#include <cassert>

#include <QDebug>
#include <QTextCursor>
#include <QTextTableCell>

//static char const *nodeToStr(cmark_node_type type)
//{
//    switch (type)
//    {
//    case CMARK_NODE_DOCUMENT:
//        return "CMARK_NODE_DOCUMENT";
//    case CMARK_NODE_BLOCK_QUOTE:
//        return "CMARK_NODE_BLOCK_QUOTE";
//    case CMARK_NODE_LIST:
//        return "CMARK_NODE_LIST";
//    case CMARK_NODE_ITEM:
//        return "CMARK_NODE_ITEM";
//    case CMARK_NODE_CODE_BLOCK:
//        return "CMARK_NODE_CODE_BLOCK";
//    case CMARK_NODE_HTML_BLOCK:
//        return "CMARK_NODE_HTML_BLOCK";
//    case CMARK_NODE_CUSTOM_BLOCK:
//        return "CMARK_NODE_CUSTOM_BLOCK";
//    case CMARK_NODE_PARAGRAPH:
//        return "CMARK_NODE_PARAGRAPH";
//    case CMARK_NODE_HEADING:
//        return "CMARK_NODE_HEADING";
//    case CMARK_NODE_THEMATIC_BREAK:
//        return "CMARK_NODE_THEMATIC_BREAK";
//    case CMARK_NODE_TEXT:
//        return "CMARK_NODE_TEXT";
//    case CMARK_NODE_SOFTBREAK:
//        return "CMARK_NODE_SOFTBREAK";
//    case CMARK_NODE_LINEBREAK:
//        return "CMARK_NODE_LINEBREAK";
//    case CMARK_NODE_CODE:
//        return "CMARK_NODE_CODE";
//    case CMARK_NODE_HTML_INLINE:
//        return "CMARK_NODE_HTML_INLINE";
//    case CMARK_NODE_CUSTOM_INLINE:
//        return "CMARK_NODE_CUSTOM_INLINE";
//    case CMARK_NODE_EMPH:
//        return "CMARK_NODE_EMPH";
//    case CMARK_NODE_STRONG:
//        return "CMARK_NODE_STRONG";
//    case CMARK_NODE_LINK:
//        return "CMARK_NODE_LINK";
//    case CMARK_NODE_IMAGE:
//        return "CMARK_NODE_IMAGE";
//    }
//    return "UNKNOWN";
//};

struct RenderState
{
    QTextCursor cursor;

    QUrl root_url;
    DocumentStyle const *style;
    DocumentOutlineModel *outline;
    TextStyleInstance text_style;

    QString &page_title;

    bool centre_first_h1;
    bool suppress_next_block = true;

    void emitNewBlock() {
        if(not suppress_next_block) {
            cursor.insertBlock();
        }
        suppress_next_block = false;
    }
};

static void renderNode(RenderState &state, cmark_node &node, const QTextCharFormat &current_format, int listIndent = 0);

static void renderChildren(RenderState &state, cmark_node & node, const QTextCharFormat &current_format, int listIndent = 0)
{
    for (auto child = cmark_node_first_child(&node); child != nullptr; child = cmark_node_next(child))
    {
        renderNode(state, *child, current_format, listIndent);
    }
}

/* Leaf Nodes
* * CMARK_NODE_HTML_BLOCK
* * CMARK_NODE_THEMATIC_BREAK
* * CMARK_NODE_CODE_BLOCK
* * CMARK_NODE_TEXT
* * CMARK_NODE_SOFTBREAK
* * CMARK_NODE_LINEBREAK
* * CMARK_NODE_CODE
* * CMARK_NODE_HTML_INLINE
*/

static QString extractNodeText(cmark_node &node)
{
    const char *data = cmark_node_get_literal(&node);
    QByteArray array(data);
    return renderhelpers::replace_quotes(array);
}

static void resetFormatting(RenderState &state, QTextBlockFormat defaultFormat = QTextBlockFormat())
{
    if (defaultFormat.isEmpty())
        defaultFormat = state.text_style.standard_format;
    state.emitNewBlock();
    state.suppress_next_block = true;
    state.cursor.setBlockFormat(defaultFormat);
}

static void renderNode(RenderState &state, cmark_node & node, const QTextCharFormat &current_format, int listIndent)
{
    auto & cursor = state.cursor;

    switch (cmark_node_get_type(&node))
    {
    case CMARK_NODE_DOCUMENT:
    {
        renderChildren(state, node, current_format);
        break;
    }

    case CMARK_NODE_BLOCK_QUOTE:
    {
        state.emitNewBlock();
        state.suppress_next_block = true;

        QTextTable *table = cursor.insertTable(1, 1, state.text_style.blockquote_tableformat);
        cursor.setBlockFormat(state.text_style.blockquote_format);
        QTextTableCell cell = table->cellAt(0, 0);
        cell.setFormat(state.text_style.blockquote);

        renderChildren(state, node, current_format);

        cursor.movePosition(QTextCursor::NextBlock);
        break;
    }
    case CMARK_NODE_LIST:
    {
        auto fmt = cursor.blockFormat();
        QTextListFormat listFormat = state.text_style.list_format;
        listFormat.setIndent(listFormat.indent() + listIndent);
        listIndent++;

        if (cmark_node_get_list_type(&node) == CMARK_ORDERED_LIST) {
            listFormat.setStyle(QTextListFormat::ListDecimal);
        }
        cursor.insertList(listFormat);

        state.suppress_next_block = true;
        renderChildren(state, node, current_format, listIndent);

        resetFormatting(state, fmt);
        break;
    }
    case CMARK_NODE_ITEM:
    {
        renderChildren(state, node, current_format, listIndent);
        break;
    }
    case CMARK_NODE_CODE_BLOCK:
    {
        state.emitNewBlock();
        cursor.setBlockFormat(state.text_style.preformatted_format);

        QString code = extractNodeText(node);
        cursor.insertText(code, state.text_style.preformatted);

        resetFormatting(state);
        break;
    }
    case CMARK_NODE_HTML_BLOCK:
    {
        qDebug() << "CMARK_NODE_HTML_BLOCK";
        break;
    }
    case CMARK_NODE_CUSTOM_BLOCK:
    {
        qDebug() << "CMARK_NODE_CUSTOM_BLOCK";
        break;
    }
    case CMARK_NODE_PARAGRAPH:
    {
        state.emitNewBlock();
        renderChildren(state, node, current_format);
        break;
    }
    case CMARK_NODE_HEADING:
    {
        QTextCharFormat fmt = current_format;
        state.emitNewBlock();
        cursor.setBlockFormat(state.text_style.heading_format);

        switch(cmark_node_get_heading_level(&node)) {
        case 1: fmt = state.text_style.standard_h1; break;
        case 2: fmt = state.text_style.standard_h2; break;
        case 3: fmt = state.text_style.standard_h3; break;
        case 4: fmt = state.text_style.standard_h3; break;
        case 5: fmt = state.text_style.standard_h3; break;
        case 6: fmt = state.text_style.standard_h3; break;

        default: qDebug() << "heading" << cmark_node_get_heading_level(&node); break;
        }

	auto text = cmark_node_get_literal(cmark_node_first_child(&node));
        switch(cmark_node_get_heading_level(&node)) {
        case 1:
            state.outline->appendH1(text, QString { });

            // Use first heading as the page's title.
            if (state.page_title.isEmpty())
                state.page_title = text;

            if (state.centre_first_h1)
            {
                auto f = state.text_style.heading_format;
                f.setAlignment(Qt::AlignCenter);
                cursor.setBlockFormat(f);
                state.centre_first_h1 = false;
            }

            break;
        case 2: state.outline->appendH2(text, QString { }); break;
        case 3: state.outline->appendH3(text, QString { }); break;
        }

        renderChildren(state, node, fmt);
        resetFormatting(state);
        break;
    }
    case CMARK_NODE_THEMATIC_BREAK:
    {
        qDebug() << "CMARK_NODE_THEMATIC_BREAK";
        break;
    }
    case CMARK_NODE_TEXT:
    {
        cursor.insertText(extractNodeText(node), current_format);
        break;
    }
    case CMARK_NODE_SOFTBREAK:
    {
        // qDebug() << "CMARK_NODE_SOFTBREAK";
        cursor.insertText(" ", current_format);
        break;
    }
    case CMARK_NODE_LINEBREAK:
    {
        // qDebug() << "CMARK_NODE_LINEBREAK";
        state.emitNewBlock();
        break;
    }
    case CMARK_NODE_CODE:
    {
        QString code = extractNodeText(node);
        cursor.insertText(code, state.text_style.preformatted);
        break;
    }
    case CMARK_NODE_HTML_INLINE:
    {
        qDebug() << "CMARK_NODE_HTML_INLINE";
        break;
    }
    case CMARK_NODE_CUSTOM_INLINE:
    {
        qDebug() << "CMARK_NODE_CUSTOM_INLINE";
        break;
    }
    case CMARK_NODE_EMPH:
    {
        auto fmt = current_format;
        fmt.setFontItalic(true);
        renderChildren(state, node, fmt);
        break;
    }
    case CMARK_NODE_STRONG:
    {
        auto fmt = current_format;
        fmt.setFontWeight(QFont::Bold);
        renderChildren(state, node, fmt);
        break;
    }
    case CMARK_NODE_IMAGE:
    case CMARK_NODE_LINK:
    {
        QUrl absolute_url = QString::fromUtf8(cmark_node_get_url(&node));
        if(absolute_url.isRelative()) {
            absolute_url = state.root_url.resolved(absolute_url);
        }

        QTextCharFormat fmt = state.text_style.external_link;
        if (absolute_url.host() == state.root_url.host())
        {
            fmt = state.text_style.standard_link;
        }

        QString suffix = "";
        if (absolute_url.scheme() != state.root_url.scheme())
        {
            if(absolute_url.scheme() != "kristall+ctrl") {
                suffix = " [" + absolute_url.scheme().toUpper() + "]";
                fmt = state.text_style.cross_protocol_link;
            }
        }

        fmt.setAnchorHref(absolute_url.toString(QUrl::FullyEncoded));
        renderChildren(state, node, fmt);
        cursor.insertText(suffix, fmt);
        break;
    }
    default: break;
    }
}

std::unique_ptr<QTextDocument> MarkdownRenderer::render(
    QByteArray const &input,
    QUrl const &root_url,
    DocumentStyle const &style,
    DocumentOutlineModel &outline,
    QString &page_title)
{

    std::unique_ptr<cmark_node, decltype(&cmark_node_free)> md_root{
        cmark_parse_document(input.data(), input.size(), 0),
        &cmark_node_free,
    };
    if (not md_root)
        return nullptr;

    auto doc = std::make_unique<QTextDocument>();
    renderhelpers::setPageMargins(doc.get(), style.margin_h, style.margin_v);
    doc->setIndentWidth(style.indent_size);

    outline.beginBuild();

    RenderState state = {
        QTextCursor { doc.get() },
        root_url,
        &style,
        &outline,
        TextStyleInstance { style },
        page_title,
        style.centre_h1
    };

    renderNode(state, *md_root, state.text_style.standard);

    outline.endBuild();

    return doc;
}
