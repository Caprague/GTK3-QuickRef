#include <gtk/gtk.h>

#include "markdown_view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kApplicationId = "io.github.quickref.GtkQuickRef";
constexpr const char* kWindowCssClass = "quickref-window";
constexpr const char* kNotebookCssClass = "quickref-notebook";
constexpr const char* kTextViewCssClass = "quickref-content";
constexpr const char* kStatusCssClass = "quickref-status";
constexpr const char* kStatusCountCssClass = "quickref-status-count";
constexpr const char* kRefreshCssClass = "quickref-refresh";
constexpr const char* kCloseCssClass = "quickref-close";
constexpr const char* kPinCssClass = "quickref-pin";
constexpr const char* kSidebarCssClass = "quickref-sidebar";

enum class DocumentFormat {
    Text,
    Markdown,
};

struct Document {
    fs::path path;
    std::string label;
    std::string content;
    DocumentFormat format = DocumentFormat::Text;
};

struct SidebarDocumentItem {
    int page = -1;
    GtkWidget* row = nullptr;
};

struct AppState {
    GtkApplication* application = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* window_canvas = nullptr;
    GtkWidget* notebook = nullptr;
    GtkWidget* sidebar_list = nullptr;
    GtkWidget* sidebar_scroller = nullptr;
    GtkWidget* document_count = nullptr;
    GtkWidget* pin_button = nullptr;
    std::vector<SidebarDocumentItem> sidebar_items;
    int active_page = 0;
    guint last_tab_nav_key = 0;
    gint64 last_tab_nav_time = 0;
    fs::path documents_directory;
    fs::path style_file;
    int initial_width = 1200;
    int initial_height = 760;
    int initial_x = 0;
    int initial_y = 0;
    fs::path settings_file;
    double background_opacity = 0.70;
    double opacity_step = 0.05;
    double min_opacity = 0.35;
    double max_opacity = 1.0;
    bool opacity_prefix_active = false;
    bool window_focused = true;
    bool keep_above = true;
    gint64 last_pin_toggle_time = 0;
    bool syncing_sidebar_selection = false;
    std::string document_count_text;
    guint opacity_mode_timeout = 0;
    guint opacity_status_source = 0;
    guint sidebar_scroll_idle = 0;
};

gboolean on_content_key_press(
    GtkWidget* widget, GdkEventKey* event, gpointer user_data);

void show_opacity_status(AppState* state);
void adjust_background_opacity(AppState* state, double direction);
void end_opacity_mode(AppState* state);
void refresh_opacity_mode_timeout(AppState* state);
void set_sidebar_active_page(AppState* state, int page);
void scroll_sidebar_to_active_page(AppState* state);
void toggle_window_pin(AppState* state);

fs::path executable_directory()
{
    std::error_code error;
    const fs::path executable = fs::read_symlink("/proc/self/exe", error);
    if (!error) {
        return executable.parent_path();
    }

    return fs::current_path();
}

fs::path user_config_directory()
{
    const char* config_home = g_get_user_config_dir();
    if (config_home != nullptr && *config_home != '\0') {
        return fs::path(config_home) / "gtk-quickref";
    }

    const char* home = g_get_home_dir();
    if (home != nullptr && *home != '\0') {
        return fs::path(home) / ".config" / "gtk-quickref";
    }

    return fs::path(".config") / "gtk-quickref";
}

fs::path resolve_documents_directory()
{
    if (const char* configured = g_getenv("QUICKREF_DIR");
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }

    const fs::path user_pages = user_config_directory() / "pages";
    std::error_code error;
    if (fs::is_directory(user_pages, error)) {
        return user_pages;
    }

    const fs::path executable_pages = executable_directory() / "pages";
    if (fs::is_directory(executable_pages, error)) {
        return executable_pages;
    }

    const fs::path parent_pages = executable_directory().parent_path() / "pages";
    if (fs::is_directory(parent_pages, error)) {
        return parent_pages;
    }

    return executable_pages;
}

fs::path resolve_settings_file()
{
    if (const char* configured = g_getenv("QUICKREF_CONFIG");
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }

    std::error_code error;
    const fs::path user_settings = user_config_directory() / "settings.conf";
    if (fs::is_regular_file(user_settings, error)) {
        return user_settings;
    }

    const fs::path executable_settings =
        executable_directory() / "settings.conf";
    if (fs::is_regular_file(executable_settings, error)) {
        return executable_settings;
    }

    const fs::path parent_settings =
        executable_directory().parent_path() / "settings.conf";
    if (fs::is_regular_file(parent_settings, error)) {
        return parent_settings;
    }

    return {};
}

std::string trim_copy(const std::string& value)
{
    const auto begin = std::find_if_not(
        value.begin(), value.end(),
        [](unsigned char character) { return std::isspace(character); });
    const auto end = std::find_if_not(
        value.rbegin(), value.rend(),
        [](unsigned char character) { return std::isspace(character); })
                         .base();
    return begin < end ? std::string(begin, end) : std::string{};
}

void load_settings(AppState* state)
{
    state->settings_file = resolve_settings_file();
    if (state->settings_file.empty()) {
        return;
    }

    char* data = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    if (!g_file_get_contents(
            state->settings_file.c_str(), &data, &length, &error)) {
        if (error != nullptr) {
            g_warning(
                "无法读取设置 %s: %s",
                state->settings_file.c_str(), error->message);
            g_error_free(error);
        }
        return;
    }

    std::string contents(data, length);
    g_free(data);

    std::size_t position = 0;
    while (position < contents.size()) {
        const std::size_t line_end = contents.find('\n', position);
        const std::string line = trim_copy(contents.substr(
            position,
            line_end == std::string::npos
                ? std::string::npos
                : line_end - position));
        position =
            line_end == std::string::npos ? contents.size() : line_end + 1;

        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim_copy(line.substr(0, separator));
        const std::string value = trim_copy(line.substr(separator + 1));
        char* number_end = nullptr;
        const double number = g_ascii_strtod(value.c_str(), &number_end);
        if (number_end == value.c_str() || *number_end != '\0') {
            continue;
        }

        if (key == "background_opacity") {
            state->background_opacity = number;
        } else if (key == "opacity_step") {
            state->opacity_step = number;
        } else if (key == "min_opacity") {
            state->min_opacity = number;
        } else if (key == "max_opacity") {
            state->max_opacity = number;
        }
    }

    state->min_opacity = std::clamp(state->min_opacity, 0.1, 1.0);
    state->max_opacity =
        std::clamp(state->max_opacity, state->min_opacity, 1.0);
    state->opacity_step =
        std::clamp(state->opacity_step, 0.01, 0.25);
    state->background_opacity = std::clamp(
        state->background_opacity,
        state->min_opacity,
        state->max_opacity);
}

fs::path resolve_style_file()
{
    if (const char* configured = g_getenv("QUICKREF_STYLE");
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }

    const fs::path user_style = user_config_directory() / "style.css";
    std::error_code error;
    if (fs::is_regular_file(user_style, error)) {
        return user_style;
    }

    const fs::path executable_style = executable_directory() / "style.css";
    if (fs::is_regular_file(executable_style, error)) {
        return executable_style;
    }

    const fs::path parent_style =
        executable_directory().parent_path() / "style.css";
    if (fs::is_regular_file(parent_style, error)) {
        return parent_style;
    }

    return {};
}

std::string lowercase_extension(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension;
}

std::string make_tab_label(const fs::path& path, const fs::path& root)
{
    std::error_code error;
    fs::path relative = fs::relative(path, root, error);
    if (error || relative.empty()) {
        relative = path.filename();
    }

    relative.replace_extension();
    return relative.generic_string();
}

std::optional<std::string> read_text_file(const fs::path& path)
{
    char* data = nullptr;
    gsize length = 0;
    GError* error = nullptr;

    if (!g_file_get_contents(path.c_str(), &data, &length, &error)) {
        if (error != nullptr) {
            g_warning("无法读取文档 %s: %s", path.c_str(), error->message);
            g_error_free(error);
        }
        return std::nullopt;
    }

    std::string content(data, length);
    g_free(data);

    if (!g_utf8_validate(content.c_str(), static_cast<gssize>(content.size()), nullptr)) {
        char* repaired = g_utf8_make_valid(content.c_str(), static_cast<gssize>(content.size()));
        content = repaired;
        g_free(repaired);
    }

    return content;
}

std::vector<Document> scan_documents(const fs::path& directory)
{
    std::vector<Document> documents;
    std::error_code error;

    if (!fs::is_directory(directory, error)) {
        return documents;
    }

    fs::recursive_directory_iterator iterator(
        directory, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;

    while (!error && iterator != end) {
        const fs::directory_entry entry = *iterator;
        iterator.increment(error);

        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            continue;
        }
        const std::string extension = lowercase_extension(entry.path());
        DocumentFormat format = DocumentFormat::Text;
        if (extension == ".md" || extension == ".markdown") {
            format = DocumentFormat::Markdown;
        } else if (extension != ".txt") {
            continue;
        }

        std::optional<std::string> content = read_text_file(entry.path());
        if (!content.has_value()) {
            continue;
        }

        documents.push_back(Document{
            entry.path(),
            make_tab_label(entry.path(), directory),
            std::move(*content),
            format,
        });
    }

    if (error) {
        g_warning("扫描文档目录时出错: %s", error.message().c_str());
    }

    std::sort(documents.begin(), documents.end(),
              [](const Document& left, const Document& right) {
                  const int by_label =
                      g_utf8_collate(left.label.c_str(), right.label.c_str());
                  if (by_label != 0) {
                      return by_label < 0;
                  }
                  return left.path.generic_string() < right.path.generic_string();
              });

    std::unordered_map<std::string, int> label_counts;
    for (const Document& document : documents) {
        ++label_counts[document.label];
    }
    std::unordered_map<std::string, int> label_occurrences;
    for (Document& document : documents) {
        if (label_counts[document.label] > 1) {
            const int occurrence = ++label_occurrences[document.label];
            std::string extension = lowercase_extension(document.path);
            if (!extension.empty() && extension.front() == '.') {
                extension.erase(extension.begin());
            }
            document.label +=
                " [" + extension + (occurrence > 1 ? " " + std::to_string(occurrence) : "") + "]";
        }
    }

    return documents;
}

std::string active_tab_label(AppState* state)
{
    if (state->notebook == nullptr) {
        return {};
    }

    const int page = state->active_page;
    if (page < 0 ||
        page >= gtk_notebook_get_n_pages(GTK_NOTEBOOK(state->notebook))) {
        return {};
    }

    GtkWidget* tab_label =
        gtk_notebook_get_tab_label(GTK_NOTEBOOK(state->notebook),
                                   gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook), page));
    if (tab_label == nullptr || !GTK_IS_LABEL(tab_label)) {
        return {};
    }

    return gtk_label_get_text(GTK_LABEL(tab_label));
}

GtkWidget* current_scroller(AppState* state)
{
    if (state->notebook == nullptr) {
        return nullptr;
    }

    const int page = state->active_page;
    if (page < 0) {
        return nullptr;
    }

    return gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook), page);
}

GtkWidget* current_focus_widget(AppState* state)
{
    GtkWidget* page = current_scroller(state);
    if (page == nullptr) {
        return nullptr;
    }

    return GTK_WIDGET(
        g_object_get_data(G_OBJECT(page), "quickref-focus-widget"));
}

void focus_current_content(AppState* state)
{
    GtkWidget* focus_widget = current_focus_widget(state);
    if (focus_widget != nullptr) {
        gtk_widget_grab_focus(focus_widget);
    }
}

void select_tab(AppState* state, int requested_page)
{
    if (state->notebook == nullptr) {
        return;
    }

    const int page_count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(state->notebook));
    if (page_count <= 0) {
        return;
    }

    int page = requested_page;
    if (page < 0) {
        page = page_count - 1;
    } else if (page >= page_count) {
        page = 0;
    }

    state->active_page = page;
    set_sidebar_active_page(state, page);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(state->notebook), page);
    focus_current_content(state);
}

void select_relative_tab(AppState* state, int offset)
{
    const int page_count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(state->notebook));
    if (page_count <= 0) {
        return;
    }

    const int current = state->active_page;
    const int next = ((current + offset) % page_count + page_count) % page_count;
    select_tab(state, next);
}

void scroll_current(AppState* state, double amount)
{
    GtkWidget* page = current_scroller(state);
    if (page == nullptr) {
        return;
    }

    if (!GTK_IS_SCROLLED_WINDOW(page)) {
        return;
    }

    GtkAdjustment* adjustment =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page));
    if (adjustment == nullptr) {
        return;
    }

    const gdouble value = gtk_adjustment_get_value(adjustment);
    gdouble step = gtk_adjustment_get_step_increment(adjustment);
    if (step <= 0.0) {
        step = 28.0;
    }

    double target = value + amount * step;
    if (amount >= G_MAXDOUBLE) {
        target = gtk_adjustment_get_upper(adjustment);
    } else if (amount <= -G_MAXDOUBLE) {
        target = 0.0;
    }

    gtk_adjustment_set_value(adjustment, target);
}

std::pair<std::string, std::string> sidebar_group_and_title(
    const fs::path& path, const fs::path& root, const std::string& fallback)
{
    std::error_code error;
    const fs::path relative = fs::relative(path, root, error);
    if (error || relative.empty()) {
        return {"文档", fallback};
    }

    const fs::path parent = relative.parent_path();
    if (parent.empty()) {
        return {"文档", fallback};
    }

    const std::string group = parent.generic_string();
    fs::path child = relative;
    child.replace_extension();
    const std::string title = child.filename().generic_string();
    return {group, title};
}

struct SidebarClickData {
    AppState* state = nullptr;
    int page = -1;
};

gboolean scroll_sidebar_idle(gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->sidebar_scroll_idle = 0;
    scroll_sidebar_to_active_page(state);
    return G_SOURCE_REMOVE;
}

void schedule_sidebar_scroll(AppState* state)
{
    if (state->sidebar_scroll_idle != 0) {
        return;
    }

    state->sidebar_scroll_idle =
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            scroll_sidebar_idle,
            state,
            nullptr);
}

void scroll_sidebar_to_active_page(AppState* state)
{
    if (state->sidebar_scroller == nullptr ||
        state->sidebar_list == nullptr) {
        return;
    }

    GtkWidget* active_row = nullptr;
    for (const SidebarDocumentItem& item : state->sidebar_items) {
        if (item.page == state->active_page) {
            active_row = item.row;
            break;
        }
    }
    if (active_row == nullptr) {
        return;
    }

    int x = 0;
    int y = 0;
    if (!gtk_widget_translate_coordinates(
            active_row, state->sidebar_list, 0, 0, &x, &y)) {
        return;
    }

    const int row_height = gtk_widget_get_allocated_height(active_row);
    if (row_height <= 0) {
        return;
    }

    GtkAdjustment* adjustment =
        gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(state->sidebar_scroller));
    if (adjustment == nullptr) {
        return;
    }

    const double lower = gtk_adjustment_get_lower(adjustment);
    const double upper = gtk_adjustment_get_upper(adjustment);
    const double page_size = gtk_adjustment_get_page_size(adjustment);
    if (page_size <= 0.0 || upper <= page_size) {
        return;
    }

    constexpr double kMargin = 6.0;
    const double value = gtk_adjustment_get_value(adjustment);
    double target = value;
    if (y < value + kMargin) {
        target = y - kMargin;
    } else if (y + row_height > value + page_size - kMargin) {
        target = y + row_height - page_size + kMargin;
    } else {
        return;
    }

    const double maximum = upper - page_size;
    target = std::clamp(target, lower, maximum);
    gtk_adjustment_set_value(adjustment, target);
}

void set_sidebar_active_page(AppState* state, int page)
{
    for (SidebarDocumentItem& item : state->sidebar_items) {
        GtkStyleContext* context =
            gtk_widget_get_style_context(item.row);
        if (item.page == page) {
            gtk_style_context_add_class(
                context, "quickref-sidebar-active");
        } else {
            gtk_style_context_remove_class(
                context, "quickref-sidebar-active");
        }
    }

    schedule_sidebar_scroll(state);
}

void on_sidebar_document_clicked(GtkButton*, gpointer user_data)
{
    auto* click = static_cast<SidebarClickData*>(user_data);
    set_sidebar_active_page(click->state, click->page);
    select_tab(click->state, click->page);
}

void append_sidebar_row(
    AppState* state, const std::string& title, const std::string& path,
    int page, bool category)
{
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget* label = gtk_label_new(title.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    if (category) {
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 8);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(label),
            "quickref-sidebar-category");
        gtk_container_add(GTK_CONTAINER(row), label);
    } else {
        GtkWidget* button = gtk_button_new_with_label(title.c_str());
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        gtk_widget_set_focus_on_click(button, FALSE);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(button),
            "quickref-sidebar-document");
        if (GtkWidget* button_label =
                gtk_bin_get_child(GTK_BIN(button));
            button_label != nullptr && GTK_IS_LABEL(button_label)) {
            gtk_label_set_xalign(GTK_LABEL(button_label), 0.0f);
        }
        gtk_widget_set_margin_start(button, 8);
        gtk_widget_set_margin_end(button, 8);
        if (!path.empty()) {
            gtk_widget_set_tooltip_text(button, path.c_str());
        }
        auto* click = new SidebarClickData{state, page};
        g_signal_connect_data(
            button, "clicked",
            G_CALLBACK(on_sidebar_document_clicked), click,
            [](gpointer data, GClosure*) {
                delete static_cast<SidebarClickData*>(data);
            },
            static_cast<GConnectFlags>(0));
        gtk_container_add(GTK_CONTAINER(row), button);
        state->sidebar_items.push_back({page, button});
    }
    gtk_list_box_insert(GTK_LIST_BOX(state->sidebar_list), row, -1);
}

void rebuild_sidebar(
    AppState* state, const std::vector<Document>& documents)
{
    if (state->sidebar_list == nullptr) {
        return;
    }

    state->syncing_sidebar_selection = true;
    state->sidebar_items.clear();
    GList* children =
        gtk_container_get_children(GTK_CONTAINER(state->sidebar_list));
    for (GList* item = children; item != nullptr; item = item->next) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
    }
    g_list_free(children);

    if (documents.empty()) {
        append_sidebar_row(state, "无文档", "", 0, false);
    } else {
        std::unordered_map<std::string, bool> emitted_categories;
        for (std::size_t page = 0; page < documents.size(); ++page) {
            const Document& document = documents[page];
            const auto [group, title] = sidebar_group_and_title(
                document.path, state->documents_directory, document.label);

            if (group != "文档" && !emitted_categories[group]) {
                append_sidebar_row(state, group, "", -1, true);
                emitted_categories[group] = true;
            }
            append_sidebar_row(
                state, title, document.path.string(),
                static_cast<int>(page), false);
        }
    }
    state->syncing_sidebar_selection = false;
    gtk_widget_show_all(state->sidebar_list);
}

void sync_sidebar_with_page(AppState* state)
{
    if (state->sidebar_list == nullptr ||
        state->notebook == nullptr ||
        state->syncing_sidebar_selection) {
        return;
    }

    const int page = state->active_page;
    if (page < 0) {
        return;
    }

    state->syncing_sidebar_selection = true;
    set_sidebar_active_page(state, page);
    state->syncing_sidebar_selection = false;
}

gboolean sync_sidebar_idle(gpointer user_data)
{
    sync_sidebar_with_page(static_cast<AppState*>(user_data));
    return G_SOURCE_REMOVE;
}

void rebuild_notebook(AppState* state)
{
    const std::string previously_selected = active_tab_label(state);

    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(state->notebook)) > 0) {
        GtkWidget* page =
            gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook), 0);
        gtk_widget_destroy(page);
    }

    const std::vector<Document> documents = scan_documents(state->documents_directory);

    if (documents.empty()) {
        GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

        GtkWidget* text_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 28);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 28);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 24);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 24);
        gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(text_view), 2);
        gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(text_view), 5);
        gtk_widget_add_events(text_view, GDK_KEY_PRESS_MASK);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(text_view), kTextViewCssClass);
        g_signal_connect(
            text_view, "key-press-event",
            G_CALLBACK(on_content_key_press), state);

        const std::string message =
            "没有找到 .txt 文档。\n\n"
            "请把文档放入以下目录，然后按 F5 重新扫描：\n" +
            state->documents_directory.string() +
            "\n\n每个 .txt 文件会自动对应一个标签页。";
        GtkTextBuffer* buffer =
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
        gtk_text_buffer_set_text(buffer, message.c_str(), -1);

        gtk_container_add(GTK_CONTAINER(scroller), text_view);
        g_object_set_data(G_OBJECT(scroller), "quickref-text-view", text_view);
        g_object_set_data(
            G_OBJECT(scroller), "quickref-focus-widget", text_view);

        GtkWidget* label = gtk_label_new("无文档");
        gtk_notebook_append_page(
            GTK_NOTEBOOK(state->notebook), scroller, label);
    } else {
        for (const Document& document : documents) {
            GtkWidget* page_widget = nullptr;

            if (document.format == DocumentFormat::Markdown) {
                page_widget = create_markdown_view(document.content);
                g_object_set_data(
                    G_OBJECT(page_widget), "quickref-focus-widget",
                    page_widget);
            } else {
                GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
                gtk_scrolled_window_set_policy(
                    GTK_SCROLLED_WINDOW(scroller),
                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

                GtkWidget* text_view = gtk_text_view_new();
                gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
                gtk_text_view_set_cursor_visible(
                    GTK_TEXT_VIEW(text_view), FALSE);
                gtk_text_view_set_wrap_mode(
                    GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
                gtk_text_view_set_monospace(
                    GTK_TEXT_VIEW(text_view), TRUE);
                gtk_text_view_set_left_margin(
                    GTK_TEXT_VIEW(text_view), 28);
                gtk_text_view_set_right_margin(
                    GTK_TEXT_VIEW(text_view), 28);
                gtk_text_view_set_top_margin(
                    GTK_TEXT_VIEW(text_view), 24);
                gtk_text_view_set_bottom_margin(
                    GTK_TEXT_VIEW(text_view), 24);
                gtk_text_view_set_pixels_above_lines(
                    GTK_TEXT_VIEW(text_view), 2);
                gtk_text_view_set_pixels_below_lines(
                    GTK_TEXT_VIEW(text_view), 5);
                gtk_style_context_add_class(
                    gtk_widget_get_style_context(text_view),
                    kTextViewCssClass);

                GtkTextBuffer* buffer =
                    gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
                gtk_text_buffer_set_text(
                    buffer, document.content.c_str(),
                    static_cast<int>(document.content.size()));
                if (g_getenv("QUICKREF_DEBUG") != nullptr) {
                    g_printerr(
                        "文本 %s: %zu 字节，缓冲区 %d 字符\n",
                        document.label.c_str(), document.content.size(),
                        gtk_text_buffer_get_char_count(buffer));
                }

                gtk_container_add(GTK_CONTAINER(scroller), text_view);
                g_object_set_data(
                    G_OBJECT(scroller), "quickref-text-view", text_view);
                g_object_set_data(
                    G_OBJECT(scroller), "quickref-focus-widget", text_view);
                page_widget = scroller;
            }

            gtk_widget_add_events(page_widget, GDK_KEY_PRESS_MASK);
            g_signal_connect(
                page_widget, "key-press-event",
                G_CALLBACK(on_content_key_press), state);
            g_object_set_data_full(
                G_OBJECT(page_widget), "quickref-document-path",
                g_strdup(document.path.c_str()), g_free);

            if (g_getenv("QUICKREF_DEBUG") != nullptr &&
                document.format == DocumentFormat::Markdown) {
                g_printerr(
                    "Markdown %s: %zu 字节\n",
                    document.label.c_str(), document.content.size());
            }

            GtkWidget* label = gtk_label_new(document.label.c_str());
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_widget_set_tooltip_text(label, document.path.c_str());
            gtk_notebook_append_page(
                GTK_NOTEBOOK(state->notebook), page_widget, label);
        }
    }

    const std::string count_text =
        std::to_string(documents.size()) + " 个文档";
    state->document_count_text = count_text;
    if (state->opacity_status_source != 0) {
        g_source_remove(state->opacity_status_source);
        state->opacity_status_source = 0;
    }
    gtk_label_set_text(GTK_LABEL(state->document_count), count_text.c_str());
    if (state->window != nullptr && GTK_IS_WINDOW(state->window)) {
        const std::string window_title = "快捷操作提示 · " + count_text;
        gtk_window_set_title(GTK_WINDOW(state->window), window_title.c_str());
    }

    int page_to_select = 0;
    if (!previously_selected.empty()) {
        for (int page = 0;
             page < gtk_notebook_get_n_pages(GTK_NOTEBOOK(state->notebook));
             ++page) {
            GtkWidget* tab_label = gtk_notebook_get_tab_label(
                GTK_NOTEBOOK(state->notebook),
                gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook), page));
            if (tab_label != nullptr && GTK_IS_LABEL(tab_label) &&
                previously_selected == gtk_label_get_text(GTK_LABEL(tab_label))) {
                page_to_select = page;
                break;
            }
        }
    }

    rebuild_sidebar(state, documents);
    gtk_widget_show_all(state->notebook);
    state->active_page = page_to_select;
    gtk_notebook_set_current_page(
        GTK_NOTEBOOK(state->notebook), page_to_select);
    set_sidebar_active_page(state, page_to_select);
    g_idle_add(sync_sidebar_idle, state);
    focus_current_content(state);
}

void close_window(AppState* state)
{
    if (state->window != nullptr) {
        gtk_widget_destroy(state->window);
    }
}

bool is_modifier(GdkModifierType state, GdkModifierType mask)
{
    return (state & mask) == mask;
}

bool is_modifier_key(guint keyval)
{
    switch (keyval) {
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R:
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R:
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R:
        case GDK_KEY_Meta_L:
        case GDK_KEY_Meta_R:
        case GDK_KEY_Hyper_L:
        case GDK_KEY_Hyper_R:
        case GDK_KEY_Caps_Lock:
        case GDK_KEY_Num_Lock:
            return true;
        default:
            return false;
    }
}

gboolean handle_navigation_key(
    GdkEventKey* event, AppState* state, bool allow_scrolling)
{
    const bool control =
        is_modifier(static_cast<GdkModifierType>(event->state), GDK_CONTROL_MASK);
    const bool alt =
        is_modifier(static_cast<GdkModifierType>(event->state), GDK_MOD1_MASK);
    const bool shift =
        is_modifier(static_cast<GdkModifierType>(event->state), GDK_SHIFT_MASK);
    if (g_getenv("QUICKREF_DEBUG") != nullptr) {
        g_printerr(
            "导航按键: keyval=%u state=%u scroll=%d active=%d\n",
            event->keyval, event->state, allow_scrolling,
            state->opacity_prefix_active);
    }

    if (state->opacity_prefix_active) {
        if (is_modifier_key(event->keyval)) {
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Escape) {
            end_opacity_mode(state);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Up ||
            event->keyval == GDK_KEY_Down) {
            adjust_background_opacity(
                state, event->keyval == GDK_KEY_Up ? 1.0 : -1.0);
            refresh_opacity_mode_timeout(state);
            return TRUE;
        }
        end_opacity_mode(state);
    }

    if (alt &&
        (event->keyval == GDK_KEY_p || event->keyval == GDK_KEY_P)) {
        toggle_window_pin(state);
        return TRUE;
    }

    if (control && shift &&
        (event->keyval == GDK_KEY_a || event->keyval == GDK_KEY_A)) {
        state->opacity_prefix_active = true;
        show_opacity_status(state);
        refresh_opacity_mode_timeout(state);
        return TRUE;
    }

    if (event->keyval == GDK_KEY_Escape ||
        (control && event->keyval == GDK_KEY_w)) {
        close_window(state);
        return TRUE;
    }

    if (event->keyval == GDK_KEY_F5 ||
        (control && event->keyval == GDK_KEY_r)) {
        rebuild_notebook(state);
        return TRUE;
    }

    if (control &&
        (event->keyval == GDK_KEY_Tab ||
         event->keyval == GDK_KEY_ISO_Left_Tab ||
         event->keyval == GDK_KEY_Page_Down ||
         event->keyval == GDK_KEY_Page_Up)) {
        const bool backwards =
            shift || event->keyval == GDK_KEY_ISO_Left_Tab ||
            event->keyval == GDK_KEY_Page_Up;
        select_relative_tab(state, backwards ? -1 : 1);
        return TRUE;
    }

    if (alt && event->keyval == GDK_KEY_Up) {
        const gint64 now = g_get_monotonic_time();
        if (state->last_tab_nav_key == event->keyval &&
            now - state->last_tab_nav_time < 30 * 1000) {
            return TRUE;
        }
        state->last_tab_nav_key = event->keyval;
        state->last_tab_nav_time = now;
        select_relative_tab(state, -1);
        return TRUE;
    }
    if (alt && event->keyval == GDK_KEY_Down) {
        const gint64 now = g_get_monotonic_time();
        if (state->last_tab_nav_key == event->keyval &&
            now - state->last_tab_nav_time < 30 * 1000) {
            return TRUE;
        }
        state->last_tab_nav_key = event->keyval;
        state->last_tab_nav_time = now;
        select_relative_tab(state, 1);
        return TRUE;
    }

    if (alt && event->keyval >= GDK_KEY_1 && event->keyval <= GDK_KEY_9) {
        select_tab(state, static_cast<int>(event->keyval - GDK_KEY_1));
        return TRUE;
    }

    if (allow_scrolling) {
        if (!control && !alt &&
            (event->keyval == GDK_KEY_Up ||
             event->keyval == GDK_KEY_KP_Up)) {
            scroll_current(state, -1.0);
            return TRUE;
        }
        if (!control && !alt &&
            (event->keyval == GDK_KEY_Down ||
             event->keyval == GDK_KEY_KP_Down)) {
            scroll_current(state, 1.0);
            return TRUE;
        }
        if (!control && !alt && event->keyval == GDK_KEY_j) {
            scroll_current(state, 1.0);
            return TRUE;
        }
        if (!control && !alt && event->keyval == GDK_KEY_k) {
            scroll_current(state, -1.0);
            return TRUE;
        }
        if (!control && !alt && event->keyval == GDK_KEY_d) {
            scroll_current(state, 12.0);
            return TRUE;
        }
        if (!control && !alt && event->keyval == GDK_KEY_u) {
            scroll_current(state, -12.0);
            return TRUE;
        }
        if (!control && !alt && event->keyval == GDK_KEY_g) {
            scroll_current(state, -G_MAXDOUBLE);
            return TRUE;
        }
        if (!control && !alt && shift && event->keyval == GDK_KEY_G) {
            scroll_current(state, G_MAXDOUBLE);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Page_Down) {
            scroll_current(state, 28.0);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Page_Up) {
            scroll_current(state, -28.0);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Home) {
            scroll_current(state, -G_MAXDOUBLE);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_End) {
            scroll_current(state, G_MAXDOUBLE);
            return TRUE;
        }
    }

    return FALSE;
}

gboolean on_content_key_press(
    GtkWidget*, GdkEventKey* event, gpointer user_data)
{
    return handle_navigation_key(
        event, static_cast<AppState*>(user_data), true);
}

gboolean on_window_key_press(
    GtkWidget*, GdkEventKey* event, gpointer user_data)
{
    return handle_navigation_key(
        event, static_cast<AppState*>(user_data), true);
}

void on_notebook_switch_page(
    GtkNotebook*, GtkWidget*, guint page, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->active_page = static_cast<int>(page);
    set_sidebar_active_page(state, state->active_page);
    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer data) -> gboolean {
            focus_current_content(static_cast<AppState*>(data));
            return G_SOURCE_REMOVE;
        },
        state,
        nullptr);
}

void on_reload_clicked(GtkButton*, gpointer user_data)
{
    rebuild_notebook(static_cast<AppState*>(user_data));
}

void apply_pin_state(AppState* state)
{
    if (state->window == nullptr) {
        return;
    }

    gtk_window_set_keep_above(
        GTK_WINDOW(state->window), state->keep_above);
    if (state->pin_button != nullptr) {
        GtkStyleContext* context =
            gtk_widget_get_style_context(state->pin_button);
        if (state->keep_above) {
            gtk_style_context_add_class(context, "quickref-pin-active");
            gtk_widget_set_tooltip_text(
                state->pin_button, "取消窗口置顶（Alt+P）");
        } else {
            gtk_style_context_remove_class(context, "quickref-pin-active");
            gtk_widget_set_tooltip_text(
                state->pin_button, "窗口置顶（Alt+P）");
        }
    }
}

void on_pin_toggled(GtkToggleButton* button, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->keep_above = gtk_toggle_button_get_active(button);
    apply_pin_state(state);
}

void toggle_window_pin(AppState* state)
{
    const gint64 now = g_get_monotonic_time();
    if (now - state->last_pin_toggle_time < 50 * 1000) {
        return;
    }
    state->last_pin_toggle_time = now;

    state->keep_above = !state->keep_above;
    if (state->pin_button != nullptr) {
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(state->pin_button), state->keep_above);
    }
    apply_pin_state(state);
}

void on_pin_accel(
    GtkAccelGroup*, GObject*, guint, GdkModifierType, gpointer user_data)
{
    toggle_window_pin(static_cast<AppState*>(user_data));
}

void install_pin_accel(GtkWidget* window, AppState* state)
{
    GtkAccelGroup* group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(window), group);
    GClosure* closure = g_cclosure_new(
        G_CALLBACK(on_pin_accel), state, nullptr);
    gtk_accel_group_connect(
        group, GDK_KEY_p,
        static_cast<GdkModifierType>(GDK_MOD1_MASK),
        GTK_ACCEL_VISIBLE, closure);
    g_closure_unref(closure);
    g_object_unref(group);
}

gboolean on_window_focus_changed(
    GtkWidget*, GdkEventFocus* event, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->window_focused = event->in;
    if (state->window_canvas != nullptr) {
        gtk_widget_queue_draw(state->window_canvas);
    }
    return FALSE;
}

void on_window_size_allocate(
    GtkWidget*, GtkAllocation* allocation, gpointer)
{
    if (g_getenv("QUICKREF_DEBUG") != nullptr) {
        g_printerr(
            "窗口尺寸: %dx%d\n", allocation->width, allocation->height);
    }
}

void on_window_state_changed_debug(
    GtkWidget*, GdkEventWindowState* event, gpointer)
{
    if (g_getenv("QUICKREF_DEBUG") != nullptr) {
        g_printerr(
            "窗口状态: current=%u changed=%u\n",
            event->new_window_state, event->changed_mask);
    }
}

void on_window_destroy(GtkWidget*, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->window = nullptr;
    state->notebook = nullptr;
    state->sidebar_list = nullptr;
    state->sidebar_scroller = nullptr;
    state->sidebar_items.clear();
    state->pin_button = nullptr;
    state->document_count = nullptr;
    if (state->sidebar_scroll_idle != 0) {
        g_source_remove(state->sidebar_scroll_idle);
        state->sidebar_scroll_idle = 0;
    }
    g_application_quit(G_APPLICATION(state->application));
}

void install_css(AppState* state)
{
    GtkCssProvider* provider = gtk_css_provider_new();
    bool loaded = false;

    if (!state->style_file.empty()) {
        GError* error = nullptr;
        loaded = gtk_css_provider_load_from_path(
            provider, state->style_file.c_str(), &error);
        if (!loaded && error != nullptr) {
            g_warning(
                "无法读取样式文件 %s: %s",
                state->style_file.c_str(), error->message);
            g_error_free(error);
        }
    }

    if (!loaded) {
        const char* fallback_css =
            ".quickref-window { background-color: transparent; }"
            ".quickref-notebook, .quickref-notebook header,"
            ".quickref-notebook scrolledwindow,"
            ".quickref-notebook viewport {"
            "  background-color: transparent;"
            "}"
            ".quickref-notebook .quickref-markdown-container {"
            "  background-color: transparent;"
            "}"
            ".quickref-notebook header { padding: 8px 176px 0 10px; }"
            ".quickref-notebook {"
            "  background-color: transparent;"
            "  border: none;"
            "  margin: 0;"
            "}"
            ".quickref-sidebar {"
            "  border-right: 1px solid #292e42;"
            "  background-color: rgba(41, 46, 66, 0.34);"
            "}"
            ".quickref-sidebar-title {"
            "  color: #7dcfff;"
            "  font-size: 13px;"
            "  font-weight: 700;"
            "}"
            ".quickref-sidebar list {"
            "  background-color: transparent;"
            "}"
            ".quickref-sidebar row {"
            "  background-color: transparent;"
            "  color: #a9b1d6;"
            "}"
            ".quickref-sidebar-document {"
            "  margin: 1px 0;"
            "  padding: 6px 8px;"
            "  border: none;"
            "  border-radius: 6px;"
            "  background-color: transparent;"
            "  color: #a9b1d6;"
            "}"
            ".quickref-sidebar-document:hover {"
            "  background-color: rgba(59, 66, 97, 0.72);"
            "  color: #c0caf5;"
            "}"
            ".quickref-sidebar-document.quickref-sidebar-active {"
            "  background-color: #7aa2f7;"
            "  color: #16161e;"
            "}"
            ".quickref-sidebar-category {"
            "  color: #7dcfff;"
            "  font-size: 12px;"
            "  font-weight: 700;"
            "}"
            ".quickref-content-paned > separator {"
            "  min-width: 1px;"
            "  min-height: 1px;"
            "  border: none;"
            "  background-color: transparent;"
            "  background-image: none;"
            "  box-shadow: inset 1px 0 rgba(125, 207, 255, 0.18);"
            "}"
            ".quickref-notebook tab {"
            "  min-width: 92px;"
            "  padding: 9px 15px;"
            "  background-color: #292e42;"
            "  color: #a9b1d6;"
            "  border: none;"
            "}"
            ".quickref-notebook tab:checked {"
            "  background-color: #7aa2f7;"
            "  color: #16161e;"
            "}"
            ".quickref-content, .quickref-content text {"
            "  background-color: transparent;"
            "  color: #c0caf5;"
            "  font-family: 'JetBrainsMono Nerd Font', monospace;"
            "  font-size: 17px;"
            "}"
            ".quickref-content:selected {"
            "  background-color: #283457;"
            "  color: #c0caf5;"
            "}"
            ".quickref-status { padding: 0 10px; }"
            ".quickref-status-count {"
            "  color: #7dcfff;"
            "  background-color: rgba(41, 46, 66, 0.95);"
            "  border: 1px solid rgba(98, 230, 255, 0.45);"
            "  border-radius: 9px;"
            "  padding: 3px 9px;"
            "  font-size: 12px;"
            "}"
            ".quickref-refresh {"
            "  background: transparent;"
            "  border: none;"
            "  color: #7dcfff;"
            "  padding: 4px;"
            "}"
            ".quickref-pin {"
            "  background: transparent;"
            "  border: none;"
            "  color: #a9b1d6;"
            "  padding: 4px;"
            "}"
            ".quickref-pin:checked {"
            "  background-color: rgba(98, 230, 255, 0.22);"
            "  color: #62e6ff;"
            "}"
            ".quickref-close {"
            "  background: transparent;"
            "  border: none;"
            "  color: #f7768e;"
            "  padding: 4px;"
            "}";

        gtk_css_provider_load_from_data(provider, fallback_css, -1, nullptr);
    }

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

gboolean draw_window_background(
    GtkWidget* widget, cairo_t* cairo, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    const double width = gtk_widget_get_allocated_width(widget);
    const double height = gtk_widget_get_allocated_height(widget);
    constexpr double radius = 10.0;

    cairo_new_path(cairo);
    cairo_move_to(cairo, 1.0 + radius, 1.0);
    cairo_line_to(cairo, width - 1.0 - radius, 1.0);
    cairo_arc(
        cairo, width - 1.0 - radius, 1.0 + radius,
        radius, -M_PI / 2.0, 0.0);
    cairo_line_to(cairo, width - 1.0, height - 1.0 - radius);
    cairo_arc(
        cairo, width - 1.0 - radius, height - 1.0 - radius,
        radius, 0.0, M_PI / 2.0);
    cairo_line_to(cairo, 1.0 + radius, height - 1.0);
    cairo_arc(
        cairo, 1.0 + radius, height - 1.0 - radius,
        radius, M_PI / 2.0, M_PI);
    cairo_line_to(cairo, 1.0, 1.0 + radius);
    cairo_arc(
        cairo, 1.0 + radius, 1.0 + radius,
        radius, M_PI, M_PI * 1.5);
    cairo_close_path(cairo);

    cairo_set_source_rgba(
        cairo,
        26.0 / 255.0,
        27.0 / 255.0,
        38.0 / 255.0,
        state->background_opacity);
    cairo_fill_preserve(cairo);

    cairo_set_line_width(cairo, 2.0);
    if (state->window_focused) {
        cairo_set_source_rgb(
            cairo, 98.0 / 255.0, 230.0 / 255.0, 255.0 / 255.0);
    } else {
        cairo_set_source_rgb(
            cairo, 41.0 / 255.0, 46.0 / 255.0, 66.0 / 255.0);
    }
    cairo_stroke(cairo);
    return FALSE;
}

void apply_background_opacity(AppState* state)
{
    if (state->window_canvas != nullptr) {
        gtk_widget_queue_draw(state->window_canvas);
    }
}

gboolean restore_document_count_label(gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->opacity_status_source = 0;
    if (state->document_count != nullptr) {
        gtk_label_set_text(
            GTK_LABEL(state->document_count),
            state->document_count_text.c_str());
    }
    return G_SOURCE_REMOVE;
}

void show_opacity_status(AppState* state)
{
    if (state->document_count == nullptr) {
        return;
    }

    const int percentage =
        static_cast<int>(std::lround(state->background_opacity * 100.0));
    const std::string status =
        "不透明度 " + std::to_string(percentage) + "%";
    gtk_label_set_text(GTK_LABEL(state->document_count), status.c_str());

    if (state->opacity_status_source != 0) {
        g_source_remove(state->opacity_status_source);
    }
    state->opacity_status_source =
        g_timeout_add(1400, restore_document_count_label, state);
}

void adjust_background_opacity(AppState* state, double direction)
{
    state->background_opacity = std::clamp(
        state->background_opacity + direction * state->opacity_step,
        state->min_opacity,
        state->max_opacity);
    apply_background_opacity(state);
    show_opacity_status(state);
}

gboolean opacity_mode_timeout(gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->opacity_mode_timeout = 0;
    end_opacity_mode(state);
    return G_SOURCE_REMOVE;
}

void refresh_opacity_mode_timeout(AppState* state)
{
    if (state->opacity_mode_timeout != 0) {
        g_source_remove(state->opacity_mode_timeout);
    }
    state->opacity_mode_timeout =
        g_timeout_add(3000, opacity_mode_timeout, state);
}

void end_opacity_mode(AppState* state)
{
    state->opacity_prefix_active = false;
    if (state->opacity_mode_timeout != 0) {
        g_source_remove(state->opacity_mode_timeout);
        state->opacity_mode_timeout = 0;
    }
}

void size_and_center_window(AppState* state)
{
    GtkWidget* window = state->window;
    int screen_width = 1280;
    int screen_height = 800;
    int screen_x = 0;
    int screen_y = 0;

    GdkDisplay* display = gtk_widget_get_display(window);
    GdkMonitor* monitor = gdk_display_get_primary_monitor(display);
    if (monitor != nullptr) {
        GdkRectangle geometry{};
        gdk_monitor_get_geometry(monitor, &geometry);
        screen_width = geometry.width;
        screen_height = geometry.height;
        screen_x = geometry.x;
        screen_y = geometry.y;
    }

    const int width = std::clamp(
        static_cast<int>(screen_width * 0.64), 760,
        std::max(760, screen_width - 120));
    const int height = std::clamp(
        static_cast<int>(screen_height * 0.76), 520,
        std::max(520, screen_height - 80));

    state->initial_width = width;
    state->initial_height = height;
    state->initial_x = screen_x + (screen_width - width) / 2;
    state->initial_y = screen_y + (screen_height - height) / 2;
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), width, height);
    gtk_window_resize(GTK_WINDOW(window), width, height);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_move(GTK_WINDOW(window), state->initial_x, state->initial_y);
}

gboolean unlock_initial_window_size(gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state->window != nullptr && GTK_IS_WINDOW(state->window)) {
        gtk_widget_set_size_request(state->window, -1, -1);
        gtk_window_set_resizable(GTK_WINDOW(state->window), TRUE);
        gtk_window_unmaximize(GTK_WINDOW(state->window));
        gtk_window_resize(
            GTK_WINDOW(state->window),
            state->initial_width, state->initial_height);
        gtk_window_move(
            GTK_WINDOW(state->window), state->initial_x, state->initial_y);
    }
    return G_SOURCE_REMOVE;
}

GtkWidget* create_window(AppState* state)
{
    GtkWidget* window = gtk_application_window_new(state->application);
    state->window = window;
    install_pin_accel(window, state);

    gtk_window_set_title(GTK_WINDOW(window), "快捷操作提示");
    gtk_window_set_default_icon_name("dialog-information");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_widget_set_app_paintable(window, TRUE);

    GdkScreen* screen = gtk_widget_get_screen(window);
    if (screen != nullptr) {
        if (GdkVisual* rgba_visual = gdk_screen_get_rgba_visual(screen);
            rgba_visual != nullptr) {
            gtk_widget_set_visual(window, rgba_visual);
        }
    }

    gtk_widget_add_events(window, GDK_KEY_PRESS_MASK | GDK_FOCUS_CHANGE_MASK);
    GtkStyleContext* window_context = gtk_widget_get_style_context(window);
    gtk_style_context_add_class(window_context, kWindowCssClass);

    GtkWidget* notebook = gtk_notebook_new();
    state->notebook = notebook;
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(notebook), kNotebookCssClass);

    GtkWidget* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar, 210, -1);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(sidebar), kSidebarCssClass);

    GtkWidget* status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(status), kStatusCssClass);
    gtk_widget_set_margin_top(status, 8);
    gtk_widget_set_margin_bottom(status, 6);
    gtk_widget_set_margin_start(status, 8);
    gtk_widget_set_margin_end(status, 8);

    GtkWidget* close_button =
        gtk_button_new_from_icon_name(
            "window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(close_button, "关闭（Esc）");
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(close_button), kCloseCssClass);
    g_signal_connect_swapped(
        close_button, "clicked", G_CALLBACK(close_window), state);
    gtk_box_pack_start(GTK_BOX(status), close_button, FALSE, FALSE, 0);

    GtkWidget* reload_button =
        gtk_button_new_from_icon_name(
            "view-refresh-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(
        reload_button, "重新扫描文档目录（F5）");
    gtk_button_set_relief(GTK_BUTTON(reload_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(reload_button, FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(reload_button), kRefreshCssClass);
    g_signal_connect(
        reload_button, "clicked", G_CALLBACK(on_reload_clicked), state);
    gtk_box_pack_start(GTK_BOX(status), reload_button, FALSE, FALSE, 0);

    GtkWidget* pin_button = gtk_toggle_button_new();
    gtk_button_set_image(
        GTK_BUTTON(pin_button),
        gtk_image_new_from_icon_name(
            "view-pin-symbolic", GTK_ICON_SIZE_MENU));
    gtk_button_set_relief(GTK_BUTTON(pin_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(pin_button, FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(pin_button), kPinCssClass);
    atk_object_set_name(
        gtk_widget_get_accessible(pin_button), "置顶");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pin_button), TRUE);
    state->pin_button = pin_button;
    gtk_box_pack_start(GTK_BOX(status), pin_button, FALSE, FALSE, 0);
    g_signal_connect(
        pin_button, "toggled", G_CALLBACK(on_pin_toggled), state);
    apply_pin_state(state);

    state->document_count = gtk_label_new("扫描中…");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(state->document_count),
        kStatusCountCssClass);
    gtk_box_pack_end(
        GTK_BOX(status), state->document_count, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(sidebar), status, FALSE, FALSE, 0);

    state->sidebar_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(
        GTK_LIST_BOX(state->sidebar_list), GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(
        GTK_LIST_BOX(state->sidebar_list), TRUE);

    GtkWidget* sidebar_scroller = gtk_scrolled_window_new(nullptr, nullptr);
    state->sidebar_scroller = sidebar_scroller;
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(sidebar_scroller),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sidebar_scroller), state->sidebar_list);
    gtk_box_pack_start(
        GTK_BOX(sidebar), sidebar_scroller, TRUE, TRUE, 0);
    GtkWidget* content_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_wide_handle(GTK_PANED(content_paned), FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(content_paned),
        "quickref-content-paned");
    gtk_paned_pack1(
        GTK_PANED(content_paned), sidebar, FALSE, FALSE);
    gtk_paned_pack2(
        GTK_PANED(content_paned), notebook, TRUE, TRUE);
    gtk_paned_set_position(GTK_PANED(content_paned), 210);
    gtk_widget_set_margin_top(content_paned, 8);
    gtk_widget_set_margin_bottom(content_paned, 8);
    gtk_widget_set_margin_start(content_paned, 8);
    gtk_widget_set_margin_end(content_paned, 8);

    state->window_canvas = gtk_drawing_area_new();
    gtk_widget_set_hexpand(state->window_canvas, TRUE);
    gtk_widget_set_vexpand(state->window_canvas, TRUE);
    g_signal_connect(
        state->window_canvas, "draw",
        G_CALLBACK(draw_window_background), state);
    GtkWidget* overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), state->window_canvas);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), content_paned);
    gtk_container_add(GTK_CONTAINER(window), overlay);

    g_signal_connect(
        window, "key-press-event", G_CALLBACK(on_window_key_press), state);
    g_signal_connect(
        window, "focus-in-event",
        G_CALLBACK(on_window_focus_changed), state);
    g_signal_connect(
        window, "focus-out-event",
        G_CALLBACK(on_window_focus_changed), state);
    g_signal_connect(
        window, "size-allocate", G_CALLBACK(on_window_size_allocate), state);
    g_signal_connect(
        window, "window-state-event",
        G_CALLBACK(on_window_state_changed_debug), state);
    g_signal_connect(
        window, "destroy", G_CALLBACK(on_window_destroy), state);
    g_signal_connect(
        notebook, "switch-page", G_CALLBACK(on_notebook_switch_page), state);

    rebuild_notebook(state);
    size_and_center_window(state);
    return window;
}

void on_application_activate(GtkApplication* application, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    state->application = application;

    if (state->window != nullptr && GTK_IS_WINDOW(state->window)) {
        gtk_window_present(GTK_WINDOW(state->window));
        focus_current_content(state);
        return;
    }

    GtkWidget* window = create_window(state);
    gtk_application_add_window(application, GTK_WINDOW(window));
    int initial_width = 0;
    int initial_height = 0;
    gtk_window_get_default_size(
        GTK_WINDOW(window), &initial_width, &initial_height);
    gtk_widget_set_size_request(window, initial_width, initial_height);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
    select_tab(state, 0);
    g_timeout_add(180, unlock_initial_window_size, state);
}

}  // namespace

int main(int argc, char** argv)
{
    gtk_init(&argc, &argv);

    auto* state = new AppState();
    state->documents_directory = resolve_documents_directory();
    state->style_file = resolve_style_file();
    load_settings(state);

    install_css(state);

    GtkApplication* application =
        gtk_application_new(kApplicationId, G_APPLICATION_FLAGS_NONE);
    state->application = application;
    g_signal_connect(
        application, "activate", G_CALLBACK(on_application_activate), state);

    const int status = g_application_run(
        G_APPLICATION(application), argc, argv);

    if (state->opacity_status_source != 0) {
        g_source_remove(state->opacity_status_source);
    }
    if (state->opacity_mode_timeout != 0) {
        g_source_remove(state->opacity_mode_timeout);
    }
    if (state->sidebar_scroll_idle != 0) {
        g_source_remove(state->sidebar_scroll_idle);
    }
    g_object_unref(application);
    delete state;
    return status;
}
