#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "controller_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef MECCHA_APP_VERSION
#error "MECCHA_APP_VERSION must be defined by the build script"
#endif

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#define MECCHA_APP_VERSION_STR STRINGIFY(MECCHA_APP_VERSION)

namespace meccha
{
    namespace
    {
        auto wide_to_utf8(const std::wstring& value) -> std::string
        {
            if (value.empty())
                return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(std::max(0, size)), '\0');
            if (size > 0)
                WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        auto utf8_to_wide(const std::string& value) -> std::wstring
        {
            if (value.empty())
                return {};
            const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(std::max(0, size)), L'\0');
            if (size > 0)
                MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size);
            return out;
        }

        auto json_escape(const std::string& value) -> std::string
        {
            std::ostringstream out;
            for (unsigned char c : value)
            {
                switch (c)
                {
                case '\\': out << "\\\\"; break;
                case '"': out << "\\\""; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20)
                        out << "\\u" << std::hex << static_cast<int>(c) << std::dec;
                    else
                        out << static_cast<char>(c);
                }
            }
            return out.str();
        }

        auto json_string(const std::string& value) -> std::string
        {
            return std::string("\"") + json_escape(value) + "\"";
        }

        auto read_text_file(const std::filesystem::path& path) -> std::string
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        auto write_text_file(const std::filesystem::path& path, const std::string& text) -> bool
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            return static_cast<bool>(file);
        }

        auto extract_json_string(const std::string& text, const std::string& key) -> std::string
        {
            const std::string needle = std::string("\"") + key + "\":\"";
            const auto start = text.find(needle);
            if (start == std::string::npos)
                return {};
            std::string out;
            bool escape = false;
            for (std::size_t i = start + needle.size(); i < text.size(); ++i)
            {
                const char c = text[i];
                if (escape)
                {
                    switch (c)
                    {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(c); break;
                    }
                    escape = false;
                    continue;
                }
                if (c == '\\')
                {
                    escape = true;
                    continue;
                }
                if (c == '"')
                    break;
                out.push_back(c);
            }
            return out;
        }

        auto extract_json_number(const std::string& text, const std::string& key, double fallback) -> double
        {
            const std::string needle = std::string("\"") + key + "\":";
            const auto start = text.find(needle);
            if (start == std::string::npos)
                return fallback;
            const char* begin = text.c_str() + start + needle.size();
            char* end = nullptr;
            const double value = std::strtod(begin, &end);
            return end == begin || !std::isfinite(value) ? fallback : value;
        }

        auto extract_json_bool(const std::string& text, const std::string& key, bool fallback) -> bool
        {
            const std::string needle = std::string("\"") + key + "\":";
            const auto start = text.find(needle);
            if (start == std::string::npos)
                return fallback;
            auto pos = start + needle.size();
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
                ++pos;
            if (text.compare(pos, 5, "false") == 0)
                return false;
            if (text.compare(pos, 4, "true") == 0)
                return true;
            return fallback;
        }

        auto clamp_double(double value, double min_value, double max_value) -> double
        {
            if (!std::isfinite(value))
                return min_value;
            return std::min(max_value, std::max(min_value, value));
        }

        auto string_to_region_mode(const std::string& str, RegionMode fallback) -> RegionMode
        {
            if (str == "paint") return RegionMode::Paint;
            if (str == "fill") return RegionMode::Fill;
            if (str == "skip") return RegionMode::Skip;
            return fallback;
        }

        auto region_mode_to_string(RegionMode mode) -> std::string
        {
            switch (mode)
            {
            case RegionMode::Paint: return "paint";
            case RegionMode::Fill: return "fill";
            case RegionMode::Skip: return "skip";
            }
            return "paint";
        }

        auto extract_json_string_array(const std::string& text, const std::string& key) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            const std::string needle = std::string("\"") + key + "\":[";
            const auto start = text.find(needle);
            if (start == std::string::npos)
                return out;
            auto pos = start + needle.size();
            while (pos < text.size() && text[pos] != ']')
            {
                if (text[pos] == '"')
                {
                    std::string item;
                    ++pos;
                    while (pos < text.size() && text[pos] != '"')
                    {
                        item.push_back(text[pos]);
                        ++pos;
                    }
                    if (!item.empty())
                        out.push_back(item);
                }
                ++pos;
            }
            return out;
        }

        auto extract_presets(const std::string& text) -> std::vector<PresetProfile>
        {
            std::vector<PresetProfile> out;
            const std::string needle = "\"presets\":[";
            const auto start = text.find(needle);
            if (start == std::string::npos)
                return out;
            auto pos = start + needle.size();
            int bracket_count = 1;
            std::string array_content;
            while (pos < text.size() && bracket_count > 0)
            {
                const char c = text[pos];
                if (c == '[')
                    ++bracket_count;
                else if (c == ']')
                    --bracket_count;
                if (bracket_count > 0)
                    array_content.push_back(c);
                ++pos;
            }
            std::size_t obj_start = 0;
            while ((obj_start = array_content.find('{', obj_start)) != std::string::npos)
            {
                const auto obj_end = array_content.find('}', obj_start);
                if (obj_end == std::string::npos)
                    break;
                const std::string obj_str = array_content.substr(obj_start, obj_end - obj_start + 1);
                PresetProfile profile{};
                profile.name = extract_json_string(obj_str, "name");
                if (!profile.name.empty())
                {
                    PaintTuning t{};
                    t.stroke_size_texels = extract_json_number(obj_str, "stroke_size_texels", t.stroke_size_texels);
                    t.coverage_step_texels = extract_json_number(obj_str, "coverage_step_texels", t.coverage_step_texels);
                    t.side_source_max_uv = extract_json_number(obj_str, "side_source_max_uv", t.side_source_max_uv);
                    t.front_back_source_max_uv = extract_json_number(obj_str, "front_back_source_max_uv", t.front_back_source_max_uv);
                    t.front_region_mode = string_to_region_mode(extract_json_string(obj_str, "front_region_mode"), t.front_region_mode);
                    t.side_region_mode = string_to_region_mode(extract_json_string(obj_str, "side_region_mode"), t.side_region_mode);
                    t.back_region_mode = string_to_region_mode(extract_json_string(obj_str, "back_region_mode"), t.back_region_mode);
                    t.server_batch_limit = static_cast<int>(extract_json_number(obj_str, "server_batch_limit", t.server_batch_limit));
                    t.server_batch_delay_ms = static_cast<int>(extract_json_number(obj_str, "server_batch_delay_ms", t.server_batch_delay_ms));
                    t.auto_material_properties = extract_json_bool(obj_str, "auto_material_properties", t.auto_material_properties);
                    t.metallic = extract_json_number(obj_str, "metallic", t.metallic);
                    t.roughness = extract_json_number(obj_str, "roughness", t.roughness);
                    t.fill_color_r = extract_json_number(obj_str, "fill_color_r", t.fill_color_r);
                    t.fill_color_g = extract_json_number(obj_str, "fill_color_g", t.fill_color_g);
                    t.fill_color_b = extract_json_number(obj_str, "fill_color_b", t.fill_color_b);
                    t.fill_metallic = extract_json_number(obj_str, "fill_metallic", t.fill_metallic);
                    t.fill_roughness = extract_json_number(obj_str, "fill_roughness", t.fill_roughness);
                    t.allow_unsafe_paint = extract_json_bool(obj_str, "allow_unsafe_paint", t.allow_unsafe_paint);
                    profile.tuning = t;
                    out.push_back(profile);
                }
                obj_start = obj_end + 1;
            }
            return out;
        }

        auto serialize_presets(const std::vector<PresetProfile>& presets) -> std::string
        {
            std::string out = "[";
            for (std::size_t i = 0; i < presets.size(); ++i)
            {
                const auto& p = presets[i];
                out += "{\n";
                out += "      \"name\": " + json_string(p.name) + ",\n";
                out += "      \"stroke_size_texels\": " + std::to_string(p.tuning.stroke_size_texels) + ",\n";
                out += "      \"coverage_step_texels\": " + std::to_string(p.tuning.coverage_step_texels) + ",\n";
                out += "      \"side_source_max_uv\": " + std::to_string(p.tuning.side_source_max_uv) + ",\n";
                out += "      \"front_back_source_max_uv\": " + std::to_string(p.tuning.front_back_source_max_uv) + ",\n";
                out += "      \"front_region_mode\": " + json_string(region_mode_to_string(p.tuning.front_region_mode)) + ",\n";
                out += "      \"side_region_mode\": " + json_string(region_mode_to_string(p.tuning.side_region_mode)) + ",\n";
                out += "      \"back_region_mode\": " + json_string(region_mode_to_string(p.tuning.back_region_mode)) + ",\n";
                out += "      \"server_batch_limit\": " + std::to_string(p.tuning.server_batch_limit) + ",\n";
                out += "      \"server_batch_delay_ms\": " + std::to_string(p.tuning.server_batch_delay_ms) + ",\n";
                out += "      \"auto_material_properties\": " + std::string(p.tuning.auto_material_properties ? "true" : "false") + ",\n";
                out += "      \"metallic\": " + std::to_string(p.tuning.metallic) + ",\n";
                out += "      \"roughness\": " + std::to_string(p.tuning.roughness) + ",\n";
                out += "      \"fill_color_r\": " + std::to_string(p.tuning.fill_color_r) + ",\n";
                out += "      \"fill_color_g\": " + std::to_string(p.tuning.fill_color_g) + ",\n";
                out += "      \"fill_color_b\": " + std::to_string(p.tuning.fill_color_b) + ",\n";
                out += "      \"fill_metallic\": " + std::to_string(p.tuning.fill_metallic) + ",\n";
                out += "      \"fill_roughness\": " + std::to_string(p.tuning.fill_roughness) + ",\n";
                out += "      \"allow_unsafe_paint\": " + std::string(p.tuning.allow_unsafe_paint ? "true" : "false") + "\n";
                out += "    }";
                if (i + 1 < presets.size())
                    out += ",";
            }
            out += "]";
            return out;
        }

        auto serialize_color_swatches(const std::vector<std::string>& swatches) -> std::string
        {
            std::string out = "[";
            for (std::size_t i = 0; i < swatches.size(); ++i)
            {
                out += json_string(swatches[i]);
                if (i + 1 < swatches.size())
                    out += ",";
            }
            out += "]";
            return out;
        }

        auto app_version_scope() -> std::wstring
        {
            std::string scope;
            for (const unsigned char c : app_version())
            {
                if (std::isalnum(c) || c == '.' || c == '_' || c == '-')
                    scope.push_back(static_cast<char>(c));
                else
                    scope.push_back('_');
            }
            if (scope.empty())
                scope = "unversioned";
            return utf8_to_wide(scope);
        }

    }

    auto app_version() -> std::string
    {
        return MECCHA_APP_VERSION_STR;
    }

    auto default_app_dir() -> std::filesystem::path
    {
        wchar_t buffer[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
        if (size > 0 && size < std::size(buffer))
            return std::filesystem::path(buffer) / L"MecchaCamouflage" / L"versions" / app_version_scope();
        return std::filesystem::temp_directory_path() / L"MecchaCamouflage" / L"versions" / app_version_scope();
    }

    auto config_path() -> std::filesystem::path
    {
        return default_app_dir() / L"config.json";
    }

    auto default_tuning() -> PaintTuning
    {
        return PaintTuning{};
    }

    void clamp_settings(AppSettings& settings)
    {
        settings.panel_width = std::max(1100.0f, settings.panel_width);
        settings.panel_height = std::max(720.0f, settings.panel_height);
        settings.opacity = static_cast<float>(clamp_double(settings.opacity, 0.35, 1.0));
        settings.tuning.stroke_size_texels = clamp_double(settings.tuning.stroke_size_texels, 1.0, 10.0);
        settings.tuning.coverage_step_texels = clamp_double(settings.tuning.coverage_step_texels, 1.0, 10.0);
        settings.tuning.side_source_max_uv = clamp_double(settings.tuning.side_source_max_uv, 0.001, 0.50);
        settings.tuning.front_back_source_max_uv = clamp_double(settings.tuning.front_back_source_max_uv, 0.001, 2.00);
        settings.tuning.metallic = clamp_double(settings.tuning.metallic, 0.0, 1.0);
        settings.tuning.roughness = clamp_double(settings.tuning.roughness, 0.0, 1.0);
        settings.tuning.server_batch_limit = std::max(1, std::min(50, settings.tuning.server_batch_limit));
        settings.tuning.server_batch_delay_ms = std::max(50, std::min(100, settings.tuning.server_batch_delay_ms));
        settings.tuning.fill_color_r = clamp_double(settings.tuning.fill_color_r, 0.0, 1.0);
        settings.tuning.fill_color_g = clamp_double(settings.tuning.fill_color_g, 0.0, 1.0);
        settings.tuning.fill_color_b = clamp_double(settings.tuning.fill_color_b, 0.0, 1.0);
        settings.tuning.fill_metallic = clamp_double(settings.tuning.fill_metallic, 0.0, 1.0);
        settings.tuning.fill_roughness = clamp_double(settings.tuning.fill_roughness, 0.0, 1.0);
        settings.bridge_port = std::max(1024, std::min(65535, settings.bridge_port));
        if (settings.start_hotkey.empty())
            settings.start_hotkey = "F1";
        if (settings.stop_hotkey.empty())
            settings.stop_hotkey = "F4";
        if (settings.preview_hotkey.empty())
            settings.preview_hotkey = "F2";
        if (settings.unpreview_hotkey.empty())
            settings.unpreview_hotkey = "F3";
    }

    auto load_settings() -> AppSettings
    {
        AppSettings settings{};
        const std::string text = read_text_file(config_path());
        if (text.empty())
            return settings;

        const int layout_version = static_cast<int>(extract_json_number(text, "layout_version", 0.0));
        if (const auto process = extract_json_string(text, "game_process_name"); !process.empty())
            settings.game_process_name = utf8_to_wide(process);
        settings.panel_x = static_cast<float>(extract_json_number(text, "panel_x", settings.panel_x));
        settings.panel_y = static_cast<float>(extract_json_number(text, "panel_y", settings.panel_y));
        settings.panel_width = static_cast<float>(extract_json_number(text, "panel_width", settings.panel_width));
        settings.panel_height = static_cast<float>(extract_json_number(text, "panel_height", settings.panel_height));
        if (layout_version < settings.layout_version && settings.panel_width <= 1040.5f)
            settings.panel_width = 1100.0f;
        if (layout_version < settings.layout_version && settings.panel_height <= 640.5f)
            settings.panel_height = 720.0f;
        settings.always_on_top = extract_json_bool(text, "always_on_top", settings.always_on_top);
        settings.opacity = static_cast<float>(extract_json_number(text, "opacity", settings.opacity));
        if (const auto hotkey = extract_json_string(text, "start_hotkey"); !hotkey.empty())
            settings.start_hotkey = hotkey;
        else if (const auto legacy_hotkey = extract_json_string(text, "paint_hotkey"); !legacy_hotkey.empty())
            settings.start_hotkey = legacy_hotkey;
        if (const auto hotkey = extract_json_string(text, "stop_hotkey"); !hotkey.empty())
            settings.stop_hotkey = hotkey;
        if (const auto hotkey = extract_json_string(text, "preview_hotkey"); !hotkey.empty())
            settings.preview_hotkey = hotkey;
        if (const auto hotkey = extract_json_string(text, "unpreview_hotkey"); !hotkey.empty())
            settings.unpreview_hotkey = hotkey;
        if (layout_version < 23)
        {
            if (settings.start_hotkey == "F10")
                settings.start_hotkey = "F1";
            if (settings.preview_hotkey == "F8")
                settings.preview_hotkey = "F2";
            if (settings.unpreview_hotkey == "F7")
                settings.unpreview_hotkey = "F3";
            if (settings.stop_hotkey == "F9")
                settings.stop_hotkey = "F4";
        }
        settings.tuning.stroke_size_texels = extract_json_number(text, "stroke_size_texels", settings.tuning.stroke_size_texels);
        settings.tuning.coverage_step_texels = extract_json_number(text, "coverage_step_texels", settings.tuning.coverage_step_texels);
        settings.tuning.side_source_max_uv = extract_json_number(text, "side_source_max_uv", settings.tuning.side_source_max_uv);
        settings.tuning.front_back_source_max_uv = extract_json_number(text, "front_back_source_max_uv", settings.tuning.front_back_source_max_uv);
        
        settings.tuning.front_region_mode = string_to_region_mode(extract_json_string(text, "front_region_mode"), RegionMode::Fill);
        settings.tuning.side_region_mode = string_to_region_mode(extract_json_string(text, "side_region_mode"), RegionMode::Paint);
        settings.tuning.back_region_mode = string_to_region_mode(extract_json_string(text, "back_region_mode"), RegionMode::Paint);
        if (text.find("\"enable_front_paint\"") != std::string::npos && text.find("\"front_region_mode\"") == std::string::npos)
        {
            settings.tuning.front_region_mode = extract_json_bool(text, "enable_front_paint", true) ? RegionMode::Paint : RegionMode::Fill;
            settings.tuning.side_region_mode = extract_json_bool(text, "enable_side_paint", true) ? RegionMode::Paint : RegionMode::Fill;
            settings.tuning.back_region_mode = extract_json_bool(text, "enable_back_paint", true) ? RegionMode::Paint : RegionMode::Fill;
        }

        settings.tuning.auto_material_properties = extract_json_bool(text, "auto_material_properties", settings.tuning.auto_material_properties);
        settings.tuning.metallic = extract_json_number(text, "metallic", settings.tuning.metallic);
        settings.tuning.roughness = extract_json_number(text, "roughness", settings.tuning.roughness);
        if (layout_version < 20 && settings.tuning.roughness <= 0.000001)
            settings.tuning.roughness = default_tuning().roughness;
        settings.tuning.server_batch_limit = static_cast<int>(extract_json_number(text, "server_batch_limit", settings.tuning.server_batch_limit));
        settings.tuning.server_batch_delay_ms = static_cast<int>(extract_json_number(text, "server_batch_delay_ms", settings.tuning.server_batch_delay_ms));
        
        settings.tuning.fill_color_r = extract_json_number(text, "fill_color_r", settings.tuning.fill_color_r);
        settings.tuning.fill_color_g = extract_json_number(text, "fill_color_g", settings.tuning.fill_color_g);
        settings.tuning.fill_color_b = extract_json_number(text, "fill_color_b", settings.tuning.fill_color_b);
        settings.tuning.fill_metallic = extract_json_number(text, "fill_metallic", settings.tuning.fill_metallic);
        settings.tuning.fill_roughness = extract_json_number(text, "fill_roughness", settings.tuning.fill_roughness);
        settings.tuning.allow_unsafe_paint = extract_json_bool(text, "allow_unsafe_paint", settings.tuning.allow_unsafe_paint);
        settings.bridge_port = static_cast<int>(extract_json_number(text, "bridge_port", settings.bridge_port));

        settings.selected_preset_index = static_cast<int>(extract_json_number(text, "selected_preset_index", settings.selected_preset_index));
        settings.presets = extract_presets(text);
        settings.color_swatches = extract_json_string_array(text, "color_swatches");

        if (settings.color_swatches.empty())
        {
            settings.color_swatches = {
                "#FFFFFF", "#000000", "#FF0000", "#00FF00",
                "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF",
                "#808080", "#FF8000", "#800080", "#008080"
            };
        }
        if (settings.presets.empty())
        {
            PresetProfile def{};
            def.name = "Default Paint";
            def.tuning = settings.tuning;
            settings.presets.push_back(def);

            PresetProfile fast{};
            fast.name = "Fast Multiplayer Paint";
            fast.tuning = settings.tuning;
            fast.tuning.server_batch_limit = 50;
            fast.tuning.server_batch_delay_ms = 50;
            settings.presets.push_back(fast);

            PresetProfile metal{};
            metal.name = "Shiny Matte Metal";
            metal.tuning = settings.tuning;
            metal.tuning.auto_material_properties = false;
            metal.tuning.metallic = 1.0;
            metal.tuning.roughness = 0.2;
            settings.presets.push_back(metal);
        }

        clamp_settings(settings);
        return settings;
    }

    auto save_settings(const AppSettings& input) -> bool
    {
        AppSettings settings = input;
        clamp_settings(settings);
        const std::string text = std::string("{\n") +
            "  \"layout_version\": " + std::to_string(settings.layout_version) + ",\n" +
            "  \"panel_x\": " + std::to_string(settings.panel_x) + ",\n" +
            "  \"panel_y\": " + std::to_string(settings.panel_y) + ",\n" +
            "  \"panel_width\": " + std::to_string(settings.panel_width) + ",\n" +
            "  \"panel_height\": " + std::to_string(settings.panel_height) + ",\n" +
            "  \"game_process_name\": " + json_string(wide_to_utf8(settings.game_process_name)) + ",\n" +
            "  \"always_on_top\": " + std::string(settings.always_on_top ? "true" : "false") + ",\n" +
            "  \"opacity\": " + std::to_string(settings.opacity) + ",\n" +
            "  \"start_hotkey\": " + json_string(settings.start_hotkey) + ",\n" +
            "  \"stop_hotkey\": " + json_string(settings.stop_hotkey) + ",\n" +
            "  \"preview_hotkey\": " + json_string(settings.preview_hotkey) + ",\n" +
            "  \"unpreview_hotkey\": " + json_string(settings.unpreview_hotkey) + ",\n" +
            "  \"stroke_size_texels\": " + std::to_string(settings.tuning.stroke_size_texels) + ",\n" +
            "  \"coverage_step_texels\": " + std::to_string(settings.tuning.coverage_step_texels) + ",\n" +
            "  \"side_source_max_uv\": " + std::to_string(settings.tuning.side_source_max_uv) + ",\n" +
            "  \"front_back_source_max_uv\": " + std::to_string(settings.tuning.front_back_source_max_uv) + ",\n" +
            "  \"front_region_mode\": " + json_string(region_mode_to_string(settings.tuning.front_region_mode)) + ",\n" +
            "  \"side_region_mode\": " + json_string(region_mode_to_string(settings.tuning.side_region_mode)) + ",\n" +
            "  \"back_region_mode\": " + json_string(region_mode_to_string(settings.tuning.back_region_mode)) + ",\n" +
            "  \"server_batch_limit\": " + std::to_string(settings.tuning.server_batch_limit) + ",\n" +
            "  \"server_batch_delay_ms\": " + std::to_string(settings.tuning.server_batch_delay_ms) + ",\n" +
            "  \"auto_material_properties\": " + std::string(settings.tuning.auto_material_properties ? "true" : "false") + ",\n" +
            "  \"metallic\": " + std::to_string(settings.tuning.metallic) + ",\n" +
            "  \"roughness\": " + std::to_string(settings.tuning.roughness) + ",\n" +
            "  \"fill_color_r\": " + std::to_string(settings.tuning.fill_color_r) + ",\n" +
            "  \"fill_color_g\": " + std::to_string(settings.tuning.fill_color_g) + ",\n" +
            "  \"fill_color_b\": " + std::to_string(settings.tuning.fill_color_b) + ",\n" +
            "  \"fill_metallic\": " + std::to_string(settings.tuning.fill_metallic) + ",\n" +
            "  \"fill_roughness\": " + std::to_string(settings.tuning.fill_roughness) + ",\n" +
            "  \"allow_unsafe_paint\": " + std::string(settings.tuning.allow_unsafe_paint ? "true" : "false") + ",\n" +
            "  \"bridge_port\": " + std::to_string(settings.bridge_port) + ",\n" +
            "  \"selected_preset_index\": " + std::to_string(settings.selected_preset_index) + ",\n" +
            "  \"presets\": " + serialize_presets(settings.presets) + ",\n" +
            "  \"color_swatches\": " + serialize_color_swatches(settings.color_swatches) + "\n" +
            "}\n";
        const auto path = config_path();
        const auto tmp = std::filesystem::path(path.wstring() + L".tmp");
        if (!write_text_file(tmp, text))
            return false;
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(tmp, path, ec);
        }
        return !ec;
    }

}
