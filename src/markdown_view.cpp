#include "markdown_view.hpp"

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

void add_class(GtkWidget* widget, const char* css_class)
{
    gtk_style_context_add_class(
        gtk_widget_get_style_context(widget), css_class);
}

GtkWidget* make_label(const std::string& text, const char* css_class)
{
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(label), FALSE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    add_class(label, css_class);
    return label;
}

const char* literal(cmark_node* node)
{
    const char* value = cmark_node_get_literal(node);
    return value != nullptr ? value : "";
}

void append_inline(cmark_node* node, std::string& output)
{
    if (node == nullptr) {
        return;
    }

    switch (cmark_node_get_type(node)) {
        case CMARK_NODE_TEXT:
        case CMARK_NODE_CODE:
        case CMARK_NODE_HTML_INLINE:
            output += literal(node);
            return;
        case CMARK_NODE_SOFTBREAK:
            output += ' ';
            return;
        case CMARK_NODE_LINEBREAK:
            output += '\n';
            return;
        case CMARK_NODE_IMAGE: {
            output += "[图片: ";
            for (cmark_node* child = cmark_node_first_child(node);
                 child != nullptr; child = cmark_node_next(child)) {
                append_inline(child, output);
            }
            output += ']';
            return;
        }
        case CMARK_NODE_LINK: {
            const std::size_t start = output.size();
            for (cmark_node* child = cmark_node_first_child(node);
                 child != nullptr; child = cmark_node_next(child)) {
                append_inline(child, output);
            }
            const char* url = cmark_node_get_url(node);
            if (url != nullptr && *url != '\0' && output.size() == start) {
                output += url;
            }
            return;
        }
        default:
            for (cmark_node* child = cmark_node_first_child(node);
                 child != nullptr; child = cmark_node_next(child)) {
                append_inline(child, output);
            }
            return;
    }
}

std::string inline_text(cmark_node* node)
{
    std::string text;
    append_inline(node, text);
    return text;
}

void inspect_cell_content(
    cmark_node* node, int& non_code_content, int& code_count,
    std::string& code)
{
    if (node == nullptr) {
        return;
    }

    const cmark_node_type type = cmark_node_get_type(node);
    if (type == CMARK_NODE_TEXT) {
        const std::string text = literal(node);
        const bool whitespace = std::all_of(
            text.begin(), text.end(),
            [](unsigned char character) { return std::isspace(character); });
        if (!whitespace) {
            ++non_code_content;
        }
        return;
    }
    if (type == CMARK_NODE_CODE) {
        ++code_count;
        code = literal(node);
        return;
    }

    if (cmark_node_first_child(node) != nullptr) {
        for (cmark_node* child = cmark_node_first_child(node);
             child != nullptr; child = cmark_node_next(child)) {
            inspect_cell_content(child, non_code_content, code_count, code);
        }
    } else if (type != CMARK_NODE_SOFTBREAK &&
               type != CMARK_NODE_LINEBREAK) {
        ++non_code_content;
    }
}

bool is_only_code(cmark_node* cell, std::string& code)
{
    int non_code_content = 0;
    int code_count = 0;
    for (cmark_node* child = cmark_node_first_child(cell);
         child != nullptr; child = cmark_node_next(child)) {
        inspect_cell_content(
            child, non_code_content, code_count, code);
        if (non_code_content > 0 || code_count > 1) {
            return false;
        }
    }
    return code_count == 1 && !code.empty();
}

void copy_to_clipboard(const std::string& text)
{
    GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, text.c_str(), -1);
}

void on_copy_clicked(GtkButton* button, gpointer)
{
    const char* text = static_cast<const char*>(
        g_object_get_data(G_OBJECT(button), "quickref-copy-text"));
    if (text == nullptr) {
        return;
    }

    copy_to_clipboard(text);
    gtk_button_set_label(button, "已复制");
}

GtkWidget* make_copy_button(const std::string& text)
{
    GtkWidget* button = gtk_button_new_with_label("复制");
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(button, FALSE);
    add_class(button, "quickref-md-copy");
    g_object_set_data_full(
        G_OBJECT(button), "quickref-copy-text",
        g_strdup(text.c_str()), g_free);
    g_signal_connect(
        button, "clicked", G_CALLBACK(on_copy_clicked), nullptr);
    return button;
}

void configure_command_button(GtkWidget* button, const std::string& command)
{
    GtkWidget* label = gtk_bin_get_child(GTK_BIN(button));
    if (label != nullptr && GTK_IS_LABEL(label)) {
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_line_wrap_mode(
            GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    }

    gtk_widget_set_tooltip_text(button, command.c_str());
}

GtkWidget* render_paragraph(cmark_node* node)
{
    return make_label(inline_text(node), "quickref-md-paragraph");
}

GtkWidget* render_heading(cmark_node* node)
{
    const int level = cmark_node_get_header_level(node);
    const char* css_class =
        level <= 1 ? "quickref-md-h1"
        : level == 2 ? "quickref-md-h2"
                     : "quickref-md-h3";
    return make_label(inline_text(node), css_class);
}

GtkWidget* render_code_block(cmark_node* node)
{
    const char* info = cmark_node_get_fence_info(node);
    const std::string language =
        info != nullptr && *info != '\0' ? info : "code";
    const std::string code = literal(node);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(box, "quickref-md-code-block");

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    add_class(header, "quickref-md-code-header");
    GtkWidget* language_label =
        make_label(language, "quickref-md-code-language");
    gtk_label_set_selectable(GTK_LABEL(language_label), FALSE);
    gtk_box_pack_start(GTK_BOX(header), language_label, TRUE, TRUE, 0);
    gtk_box_pack_end(
        GTK_BOX(header), make_copy_button(code), FALSE, FALSE, 0);

    GtkWidget* code_label = make_label(code, "quickref-md-code");
    gtk_label_set_line_wrap(GTK_LABEL(code_label), FALSE);

    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), code_label, FALSE, FALSE, 0);
    return box;
}

GtkWidget* render_list(cmark_node* node)
{
    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(list, "quickref-md-list");

    const bool ordered =
        cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
    int number = cmark_node_get_list_start(node);
    if (number <= 0) {
        number = 1;
    }

    for (cmark_node* item = cmark_node_first_child(node);
         item != nullptr; item = cmark_node_next(item)) {
        const std::string marker =
            ordered ? std::to_string(number++) + "." : "•";
        std::string text;
        for (cmark_node* child = cmark_node_first_child(item);
             child != nullptr; child = cmark_node_next(child)) {
            const cmark_node_type type = cmark_node_get_type(child);
            if (!text.empty()) {
                text.push_back(' ');
            }
            if (type == CMARK_NODE_CODE_BLOCK) {
                text += literal(child);
            } else {
                text += inline_text(child);
            }
        }
        std::string line = marker + "  " + text;
        GtkWidget* label =
            make_label(line, "quickref-md-list-item");
        gtk_widget_set_margin_start(label, 12);
        gtk_box_pack_start(GTK_BOX(list), label, TRUE, TRUE, 0);
    }
    return list;
}

GtkWidget* render_quote(cmark_node* node)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    add_class(box, "quickref-md-quote");
    for (cmark_node* child = cmark_node_first_child(node);
         child != nullptr; child = cmark_node_next(child)) {
        GtkWidget* label = make_label(
            inline_text(child), "quickref-md-quote-text");
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    }
    return box;
}

GtkWidget* render_cell(cmark_node* cell, bool header)
{
    std::string code;
    if (!header && is_only_code(cell, code)) {
        GtkWidget* button = gtk_button_new_with_label(code.c_str());
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        gtk_widget_set_focus_on_click(button, FALSE);
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        add_class(button, "quickref-md-command");
        g_object_set_data_full(
            G_OBJECT(button), "quickref-copy-text",
            g_strdup(code.c_str()), g_free);
        configure_command_button(button, code);
        g_signal_connect(
            button, "clicked", G_CALLBACK(on_copy_clicked), nullptr);
        return button;
    }

    GtkWidget* label =
        make_label(inline_text(cell), "quickref-md-table-cell");
    if (header) {
        add_class(label, "quickref-md-table-header");
    }
    return label;
}

GtkWidget* render_table(cmark_node* table)
{
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 0);
    gtk_widget_set_hexpand(grid, TRUE);
    add_class(grid, "quickref-md-table");

    int row_index = 0;
    for (cmark_node* row = cmark_node_first_child(table);
         row != nullptr; row = cmark_node_next(row)) {
        const bool header =
            cmark_gfm_extensions_get_table_row_is_header(row) != 0;
        int column_index = 0;
        for (cmark_node* cell = cmark_node_first_child(row);
             cell != nullptr; cell = cmark_node_next(cell)) {
            GtkWidget* widget = render_cell(cell, header);
            gtk_grid_attach(
                GTK_GRID(grid), widget,
                column_index, row_index, 1, 1);
            ++column_index;
        }
        ++row_index;
    }
    return grid;
}

GtkWidget* render_block(cmark_node* node)
{
    switch (cmark_node_get_type(node)) {
        case CMARK_NODE_HEADING:
            return render_heading(node);
        case CMARK_NODE_PARAGRAPH:
            return render_paragraph(node);
        case CMARK_NODE_CODE_BLOCK:
            return render_code_block(node);
        case CMARK_NODE_BLOCK_QUOTE:
            return render_quote(node);
        case CMARK_NODE_LIST:
            return render_list(node);
        case CMARK_NODE_THEMATIC_BREAK: {
            GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            add_class(separator, "quickref-md-rule");
            return separator;
        }
        default:
            if (g_strcmp0(cmark_node_get_type_string(node), "table") == 0) {
                return render_table(node);
            }
            return make_label(inline_text(node), "quickref-md-paragraph");
    }
}

cmark_node* parse_markdown(const std::string& markdown)
{
    cmark_gfm_core_extensions_ensure_registered();
    cmark_parser* parser = cmark_parser_new(CMARK_OPT_DEFAULT);
    if (parser == nullptr) {
        return nullptr;
    }

    for (const char* extension_name :
         {"table", "strikethrough", "autolink", "tagfilter", "tasklist"}) {
        if (cmark_syntax_extension* extension =
                cmark_find_syntax_extension(extension_name);
            extension != nullptr) {
            cmark_parser_attach_syntax_extension(parser, extension);
        }
    }

    cmark_parser_feed(parser, markdown.c_str(), markdown.size());
    cmark_node* document = cmark_parser_finish(parser);
    cmark_parser_free(parser);
    return document;
}

}  // namespace

GtkWidget* create_markdown_view(const std::string& markdown)
{
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroller), FALSE);
    add_class(scroller, "quickref-markdown-container");

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(body, 20);
    gtk_widget_set_margin_bottom(body, 24);
    gtk_widget_set_margin_start(body, 28);
    gtk_widget_set_margin_end(body, 28);
    add_class(body, "quickref-md-body");

    cmark_node* document = parse_markdown(markdown);
    if (document == nullptr) {
        GtkWidget* label =
            make_label("Markdown 解析失败。", "quickref-md-paragraph");
        gtk_box_pack_start(GTK_BOX(body), label, FALSE, FALSE, 0);
    } else {
        for (cmark_node* child = cmark_node_first_child(document);
             child != nullptr; child = cmark_node_next(child)) {
            GtkWidget* widget = render_block(child);
            gtk_box_pack_start(GTK_BOX(body), widget, FALSE, FALSE, 0);
        }
        cmark_node_free(document);
    }

    gtk_container_add(GTK_CONTAINER(scroller), body);
    g_object_set_data(
        G_OBJECT(scroller), "quickref-focus-widget", scroller);
    return scroller;
}
