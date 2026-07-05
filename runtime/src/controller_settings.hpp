#pragma once

#include <filesystem>
#include <string>

namespace meccha
{
    inline constexpr wchar_t DefaultGameProcessName[] = L"PenguinHotel-Win64-Shipping.exe";

    enum class RegionMode
    {
        Paint = 0,
        Fill = 1,
        Skip = 2
    };

    struct PaintTuning
    {
        double stroke_size_texels{6.0};
        double coverage_step_texels{6.0};
        double side_source_max_uv{0.08};
        double front_back_source_max_uv{0.45};
        RegionMode front_region_mode{RegionMode::Fill};
        RegionMode side_region_mode{RegionMode::Paint};
        RegionMode back_region_mode{RegionMode::Paint};
        int server_batch_limit{50};
        int server_batch_delay_ms{150};
        bool auto_material_properties{false};
        double metallic{0.0};
        double roughness{1.0};
        double fill_color_r{1.0};
        double fill_color_g{1.0};
        double fill_color_b{1.0};
        double fill_metallic{1.0};
        double fill_roughness{0.0};
        bool allow_unsafe_paint{false};
    };

    struct AppSettings
    {
        int layout_version{33};
        float panel_x{-1.0f};
        float panel_y{-1.0f};
        float panel_width{1100.0f};
        float panel_height{720.0f};
        int log_retention_days{14};
        std::wstring game_process_name{DefaultGameProcessName};
        bool always_on_top{true};
        float opacity{1.0f};
        std::string start_hotkey{"F1"};
        std::string preview_hotkey{"F2"};
        std::string unpreview_hotkey{"F3"};
        std::string stop_hotkey{"F4"};
        PaintTuning tuning{};
        bool show_info{true};
        bool show_warning{true};
        bool show_error{true};
        int bridge_port{50262};
    };

    auto default_app_dir() -> std::filesystem::path;
    auto config_path() -> std::filesystem::path;
    auto app_version() -> std::string;
    auto default_tuning() -> PaintTuning;
    void clamp_settings(AppSettings& settings);
    auto load_settings() -> AppSettings;
    auto save_settings(const AppSettings& settings) -> bool;
}
