#include <absim.hpp>

#include <memory>

#include <imgui.h>
#include <nfd.hpp>

#if defined(__EMSCRIPTEN__)
#define SOKOL_GLES2
#elif defined(__APPLE__)
#define SOKOL_METAL
#elif defined(_WIN32)
#define SOKOL_D3D11
#else
#define SOKOL_GLCORE33
#endif

#define SOKOL_IMPL
#include <sokol_app.h>
#include <sokol_audio.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_time.h>
#include <sokol_imgui.h>

#include "ide_common.hpp"

static void app_frame()
{
    frame_logic();

    {
        static uint64_t pt = 0;
        simgui_frame_desc_t desc{};
        desc.delta_time = sapp_frame_duration();
        desc.dpi_scale = pixel_ratio;
        desc.width = sapp_width();
        desc.height = sapp_height();
        simgui_new_frame(&desc);
    }

    imgui_content();

    sg_pass_action action{};
    action.colors[0].action = SG_ACTION_CLEAR;
    action.colors[0].value = { 0.f, 0.f, 0.f, 1.f };
    sg_begin_default_pass(&action, sapp_width(), sapp_height());

    simgui_render();

    sg_end_pass();
    sg_commit();
}

static void app_init()
{
    stm_setup();
    NFD::Init();

    {
        sg_desc desc{};
        desc.context = sapp_sgcontext();
        sg_setup(&desc);
    }

    {
        simgui_desc_t desc{};
        desc.ini_filename = "imgui.ini";
        desc.no_default_font = true;
        simgui_setup(&desc);
    }

    {
        saudio_desc desc{};
        desc.num_channels = 1;
        desc.sample_rate = AUDIO_FREQ;
        desc.packet_frames = 2048;
        saudio_setup(&desc);
    }

    init();
    update_pixel_ratio();
    rescale_style();
    rebuild_fonts();

    //{
    //    sg_image_desc desc{};
    //    desc.width = 128;
    //    desc.height = 64;
    //    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    //    desc.min_filter = SG_FILTER_LINEAR;
    //    desc.mag_filter = SG_FILTER_NEAREST;
    //    desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    //    desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    //    desc.usage = SG_USAGE_STREAM;
    //    auto img = sg_make_image(&desc);
    //    display_texture = (texture_t)(uintptr_t)img.id;
    //    img = sg_make_image(&desc);
    //    display_buffer_texture = (texture_t)(uintptr_t)img.id;
    //}
}

static void app_event(sapp_event const* e)
{
    simgui_handle_event(e);

#ifndef __EMSCRIPTEN__
    if(e->type == SAPP_EVENTTYPE_FILES_DROPPED)
    {
        //int n = sapp_get_num_dropped_files();
        //for(int i = 0; i < n; ++i)
        //{
        //    char const* fname = sapp_get_dropped_file_path(i);
        //    std::ifstream f(fname, std::ios::binary);
        //    std::vector<uint8_t> fdata(
        //        (std::istreambuf_iterator<char>(f)),
        //        std::istreambuf_iterator<char>());
        //    load_file(fname, fdata.data(), fdata.size());
        //}
    }
#endif
}

static void app_cleanup()
{
    NFD::Quit();
    shutdown();
    saudio_shutdown();
    simgui_shutdown();
    //platform_destroy_texture(display_texture);
#ifndef ARDENS_NO_DEBUGGER
    platform_destroy_texture(display_buffer_texture);
#endif
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char** argv)
{
    sapp_desc desc{};
    desc.enable_clipboard = true;
    desc.high_dpi = true;
    desc.init_cb = app_init;
    desc.event_cb = app_event;
    desc.frame_cb = app_frame;
    desc.cleanup_cb = app_cleanup;
#ifdef __EMSCRIPTEN__
    desc.html5_canvas_name = "canvas";
#else
    desc.width = 1280;
    desc.height = 720;
    desc.window_title = "ABC IDE";
    desc.enable_dragndrop = true;
    desc.max_dropped_files = 1;
#endif
#ifdef _WIN32
    desc.win32_console_attach = true;
#endif

    // icon
    //if(ardens_icon.bytes_per_pixel == 4 && ardens_icon.width == ardens_icon.height)
    //{
    //    auto& base = icon_imgs[0];
    //    int w = ardens_icon.width;
    //    base.resize(w * w);
    //    memcpy(base.data(), ardens_icon.pixel_data, base.size() * 4);
    //    for(size_t i = 1; i < icon_imgs.size(); ++i)
    //        scale_icon(icon_imgs[i], icon_imgs[i - 1], w);
    //    for(size_t i = 0; i < icon_imgs.size(); ++i)
    //    {
    //        auto& image = desc.icon.images[i];
    //        auto const& icon = icon_imgs[i];
    //        image.width = w;
    //        image.height = w;
    //        image.pixels = { icon.data(), icon.size() * 4 };
    //        w *= 2;
    //    }
    //}

    return desc;
}

void platform_set_clipboard_text(char const* str)
{
    sapp_set_clipboard_string(str);
}

void platform_send_sound()
{
    std::vector<int16_t> buf;
    buf.swap(arduboy->cpu.sound_buffer);
    if(buf.empty())
        return;
    if(saudio_expect() <= 0)
        return;
    if(saudio_sample_rate() <= 0)
        return;
    if(saudio_suspended())
        return;

    std::vector<float> sbuf;

    double const f = double(saudio_sample_rate()) / AUDIO_FREQ;
    sbuf.resize(size_t(buf.size() * f + 0.5));
    if(sbuf.size() > saudio_expect())
        sbuf.resize(saudio_expect());

    constexpr float SOUND_GAIN = 1.f / 32768;
    for(size_t i = 0; i < sbuf.size(); ++i)
    {
        size_t j = size_t(i * f);
        if(j >= buf.size()) j = buf.size() - 1;
        sbuf[i] = float(buf[j]) * SOUND_GAIN;
    }

    if(!sbuf.empty())
        saudio_push(sbuf.data(), (int)sbuf.size());
}

uint64_t platform_get_ms_dt()
{
    static uint64_t dt_rem = 0;
    static uint64_t pt = 0;
    uint64_t dt = stm_laptime(&pt);
    dt += dt_rem;
    uint64_t ms = dt / 1000000;
    dt_rem = dt - ms * 1000000;
    return ms;
}

float platform_pixel_ratio()
{
    return sapp_dpi_scale();
}

void platform_destroy_fonts_texture()
{
    sg_destroy_image(_simgui.img);
    _simgui.img = { SG_INVALID_ID };
}

void platform_create_fonts_texture()
{
    unsigned char* font_pixels;
    int font_width, font_height;
    auto& io = ImGui::GetIO();
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    sg_image_desc img_desc{};
    img_desc.width = font_width;
    img_desc.height = font_height;
    img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    img_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    img_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    img_desc.min_filter = SG_FILTER_LINEAR;
    img_desc.mag_filter = SG_FILTER_LINEAR;
    img_desc.data.subimage[0][0].ptr = font_pixels;
    img_desc.data.subimage[0][0].size = (size_t)(font_width * font_height) * sizeof(uint32_t);
    img_desc.label = "sokol-imgui-font";
    _simgui.img = sg_make_image(&img_desc);
    io.Fonts->TexID = (ImTextureID)(uintptr_t)_simgui.img.id;
}

void platform_toggle_fullscreen()
{
#ifdef __EMSCRIPTEN__
    EmscriptenFullscreenChangeEvent e{};
    if(emscripten_get_fullscreen_status(&e) != EMSCRIPTEN_RESULT_SUCCESS)
        return;
    if(e.isFullscreen)
        emscripten_exit_fullscreen();
    else
        emscripten_request_fullscreen("#canvas", true);
#else
    sapp_toggle_fullscreen();
#endif
}
