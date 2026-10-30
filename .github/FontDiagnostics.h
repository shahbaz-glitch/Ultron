
#pragma once

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// -------------------------------------------------------------
// Optional independent FreeType diagnostics
// -------------------------------------------------------------

#if __has_include(<ft2build.h>)
    #include <ft2build.h>
    #include FT_FREETYPE_H
    #define ULTRON_FONT_DIAGNOSTICS_HAS_FREETYPE 1
#else
    #define ULTRON_FONT_DIAGNOSTICS_HAS_FREETYPE 0
#endif

namespace UltronFontDiagnostics
{

// -------------------------------------------------------------
// Configuration
// -------------------------------------------------------------

inline constexpr const char* FontPath =
    "fonts/RobotoFlex[GRAD,XOPQ,XTRA,YOPQ,YTAS,YTDE,YTFI,YTLC,YTUC,opsz,slnt,wdth,wght].ttf";

inline constexpr const char* ExpectedFamily =
    "Roboto Flex";

inline constexpr Uint64 MaximumFontSize =
    64ULL * 1024ULL * 1024ULL;

// -------------------------------------------------------------
// Diagnostic result
// -------------------------------------------------------------

struct Result
{
    bool file_opened = false;
    bool file_read = false;
    bool signature_valid = false;

    bool freetype_initialized = false;
    bool freetype_parsed = false;
    bool family_matches = false;
    bool unicode_available = false;
    bool glyphs_available = false;
    bool glyphs_rasterized = false;
    bool scalable = false;

    bool rml_probe_created = false;
    bool framebuffer_tested = false;
    bool glyphs_visible = false;
};

inline Result& GetResult()
{
    static Result result;
    return result;
}

// -------------------------------------------------------------
// Helpers
// -------------------------------------------------------------

inline bool EqualIgnoreCase(
    const std::string& a,
    const std::string& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }

    return true;
}

inline bool IsValidSignature(
    const std::array<Uint8, 4>& h)
{
    return
        (h[0] == 0x00 && h[1] == 0x01 &&
         h[2] == 0x00 && h[3] == 0x00) ||

        (h[0] == 'O' && h[1] == 'T' &&
         h[2] == 'T' && h[3] == 'O') ||

        (h[0] == 't' && h[1] == 'r' &&
         h[2] == 'u' && h[3] == 'e') ||

        (h[0] == 't' && h[1] == 'y' &&
         h[2] == 'p' && h[3] == '1') ||

        (h[0] == 't' && h[1] == 't' &&
         h[2] == 'c' && h[3] == 'f');
}

// -------------------------------------------------------------
// TEST 1: Open and completely read the font file
// -------------------------------------------------------------

inline bool TestFontFile(
    std::vector<Uint8>& data)
{
    Result& result = GetResult();

    SDL_Log("[FONT DIAGNOSTIC] TEST 1: Font asset access");

    SDL_ClearError();

    SDL_IOStream* file =
        SDL_IOFromFile(FontPath, "rb");

    if (file == nullptr)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Cannot open '%s'. SDL: %s",
            FontPath,
            SDL_GetError());

        return false;
    }

    result.file_opened = true;

    const Sint64 size = SDL_GetIOSize(file);

    if (size < 4 ||
        static_cast<Uint64>(size) > MaximumFontSize ||
        static_cast<Uint64>(size) >
            static_cast<Uint64>(
                std::numeric_limits<size_t>::max()))
    {
        SDL_CloseIO(file);

        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Invalid font size: %lld",
            static_cast<long long>(size));

        return false;
    }

    data.resize(static_cast<size_t>(size));

    size_t total_read = 0;

    while (total_read < data.size())
    {
        const size_t count = SDL_ReadIO(
            file,
            data.data() + total_read,
            data.size() - total_read);

        if (count == 0)
        {
            const std::string error = SDL_GetError();

            SDL_CloseIO(file);
            data.clear();

            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "[FONT FAIL] Incomplete read: %zu/%zu bytes. SDL: %s",
                total_read,
                static_cast<size_t>(size),
                error.c_str());

            return false;
        }

        total_read += count;
    }

    SDL_CloseIO(file);

    result.file_read = true;

    const std::array<Uint8, 4> header{
        data[0], data[1], data[2], data[3]
    };

    if (!IsValidSignature(header))
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Invalid font signature: %02X %02X %02X %02X",
            header[0], header[1], header[2], header[3]);

        return false;
    }

    result.signature_valid = true;

    SDL_Log(
        "[FONT PASS] File opened successfully.");

    SDL_Log(
        "[FONT PASS] Complete file read: %zu bytes.",
        data.size());

    SDL_Log(
        "[FONT PASS] Valid TTF/OTF signature.");

    return true;
}

// -------------------------------------------------------------
// TEST 2: Independent FreeType parsing and metadata
// -------------------------------------------------------------

#if ULTRON_FONT_DIAGNOSTICS_HAS_FREETYPE

inline bool TestFreeType(
    const std::vector<Uint8>& data)
{
    Result& result = GetResult();

    SDL_Log("[FONT DIAGNOSTIC] TEST 2: FreeType parsing");

    FT_Library library = nullptr;

    FT_Error error = FT_Init_FreeType(&library);

    if (error != 0)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] FT_Init_FreeType error: %d",
            static_cast<int>(error));

        return false;
    }

    result.freetype_initialized = true;

    SDL_Log("[FONT PASS] FreeType initialized.");

    if (data.size() >
        static_cast<size_t>(
            std::numeric_limits<FT_Long>::max()))
    {
        FT_Done_FreeType(library);

        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Font exceeds FreeType size limit.");

        return false;
    }

    FT_Face face = nullptr;

    error = FT_New_Memory_Face(
        library,
        reinterpret_cast<const FT_Byte*>(data.data()),
        static_cast<FT_Long>(data.size()),
        0,
        &face);

    if (error != 0 || face == nullptr)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] FreeType cannot parse the font. Error: %d",
            static_cast<int>(error));

        FT_Done_FreeType(library);
        return false;
    }

    result.freetype_parsed = true;

    SDL_Log("[FONT PASS] FreeType parsed font.");

    const std::string family =
        face->family_name != nullptr
            ? face->family_name
            : "";

    const std::string style =
        face->style_name != nullptr
            ? face->style_name
            : "";

    result.family_matches =
        EqualIgnoreCase(family, ExpectedFamily);

    result.scalable = FT_IS_SCALABLE(face) != 0;

    SDL_Log("[FONT INFO] Family: %s",
        family.empty() ? "(missing)" : family.c_str());

    SDL_Log("[FONT INFO] Style: %s",
        style.empty() ? "(missing)" : style.c_str());

    SDL_Log("[FONT INFO] Glyph count: %ld",
        static_cast<long>(face->num_glyphs));

    SDL_Log("[FONT INFO] Units per EM: %u",
        static_cast<unsigned>(face->units_per_EM));

    SDL_Log(
        result.family_matches
            ? "[FONT PASS] Font family matches CSS."
            : "[FONT FAIL] Font family does not match CSS.");

    SDL_Log(
        result.scalable
            ? "[FONT PASS] Scalable font."
            : "[FONT FAIL] Font is not scalable.");

    // ---------------------------------------------------------
    // Unicode character map
    // ---------------------------------------------------------

    error = FT_Select_Charmap(
        face,
        FT_ENCODING_UNICODE);

    if (error != 0)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Unicode character map unavailable: %d",
            static_cast<int>(error));

        FT_Done_Face(face);
        FT_Done_FreeType(library);

        return false;
    }

    result.unicode_available = true;

    SDL_Log("[FONT PASS] Unicode character map selected.");

    // ---------------------------------------------------------
    // Glyph availability and actual FreeType rasterization
    // ---------------------------------------------------------

    error = FT_Set_Pixel_Sizes(face, 0, 48);

    if (error != 0)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Could not set glyph size: %d",
            static_cast<int>(error));

        FT_Done_Face(face);
        FT_Done_FreeType(library);

        return false;
    }

    struct Glyph
    {
        FT_ULong codepoint;
        const char* name;
    };

    constexpr Glyph tests[] =
    {
        {0x0041, "A"},
        {0x0042, "B"},
        {0x0061, "a"},
        {0x0030, "0"},
        {0x003F, "?"},
        {0x00E9, "e-acute"}
    };

    constexpr size_t count =
        sizeof(tests) / sizeof(tests[0]);

    size_t available = 0;
    size_t rasterized = 0;

    for (const Glyph& test : tests)
    {
        const FT_UInt index =
            FT_Get_Char_Index(face, test.codepoint);

        if (index == 0)
        {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "[FONT FAIL] Missing glyph %s (U+%04lX).",
                test.name,
                static_cast<unsigned long>(test.codepoint));

            continue;
        }

        ++available;

        error = FT_Load_Glyph(
            face,
            index,
            FT_LOAD_DEFAULT);

        if (error != 0)
        {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "[FONT FAIL] FT_Load_Glyph '%s': %d",
                test.name,
                static_cast<int>(error));

            continue;
        }

        error = FT_Render_Glyph(
            face->glyph,
            FT_RENDER_MODE_NORMAL);

        if (error != 0)
        {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "[FONT FAIL] FT_Render_Glyph '%s': %d",
                test.name,
                static_cast<int>(error));

            continue;
        }

        ++rasterized;

        SDL_Log(
            "[FONT PASS] Glyph '%s' rasterized: %u x %u.",
            test.name,
            static_cast<unsigned>(face->glyph->bitmap.width),
            static_cast<unsigned>(face->glyph->bitmap.rows));
    }

    result.glyphs_available = available == count;
    result.glyphs_rasterized = rasterized == count;

    SDL_Log(
        "[FONT INFO] Glyph summary: %zu/%zu available; %zu/%zu rasterized.",
        available,
        count,
        rasterized,
        count);

    // Save all information before releasing the face.
    FT_Done_Face(face);
    FT_Done_FreeType(library);

    return
        result.freetype_parsed &&
        result.family_matches &&
        result.scalable &&
        result.unicode_available &&
        result.glyphs_available &&
        result.glyphs_rasterized;
}

#endif

// -------------------------------------------------------------
// Main preflight test
// -------------------------------------------------------------

inline bool RunPreflight()
{
    GetResult() = Result{};

    SDL_Log("==========================================");
    SDL_Log(" ULTRON FONT DIAGNOSTIC");
    SDL_Log("==========================================");

    std::vector<Uint8> data;

    if (!TestFontFile(data))
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT RESULT] Asset/file validation FAILED.");

        return false;
    }

#if ULTRON_FONT_DIAGNOSTICS_HAS_FREETYPE

    const bool freetype_ok = TestFreeType(data);

    SDL_Log(
        "[FONT RESULT] FreeType validation: %s",
        freetype_ok ? "PASS" : "FAIL");

    return freetype_ok;

#else

    SDL_LogWarn(
        SDL_LOG_CATEGORY_APPLICATION,
        "[FONT SKIP] FreeType headers are not exposed to "
        "the application target. Independent FreeType test skipped.");

    SDL_Log(
        "[FONT RESULT] File validation PASS; "
        "FreeType validation NOT TESTED.");

    return true;

#endif
}

// -------------------------------------------------------------
// TEST 3: Create an actual RmlUi text-rendering probe
// -------------------------------------------------------------

inline Rml::ElementDocument* CreateRenderProbe(
    Rml::Context* context)
{
    if (context == nullptr)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] RmlUi context is null.");

        return nullptr;
    }

    static constexpr const char* markup = R"rml(
<rml>
    <head>
        <style>
            body {
                margin: 0px;
                padding: 0px;
                background-color: transparent;
            }

            #ultron_font_diagnostic {
                position: absolute;
                top: 0px;
                left: 0px;

                width: 100%;
                height: 230px;

                padding: 12px;

                background-color: #000000;
                color: #00ff66;

                font-family: "Roboto Flex";
                font-size: 48px;
                font-weight: 400;
            }
        </style>
    </head>

    <body>
        <div id="ultron_font_diagnostic">
            ULTRON FONT TEST AaBb 0123456789 !?
        </div>
    </body>
</rml>
)rml";

    Rml::ElementDocument* document =
        context->LoadDocumentFromMemory(
            markup,
            "[ultron-font-diagnostic.rml]");

    if (document == nullptr)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Could not create RmlUi test document.");

        return nullptr;
    }

    if (document->GetElementById(
            "ultron_font_diagnostic") == nullptr)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Diagnostic text element missing.");

        document->Hide();
        return nullptr;
    }

    document->Show();

    GetResult().rml_probe_created = true;

    SDL_Log(
        "[FONT PASS] RmlUi diagnostic document created.");

    SDL_Log(
        "[FONT INFO] Expected CSS family: %s",
        ExpectedFamily);

    return document;
}

// -------------------------------------------------------------
// TEST 4: Check actual rendered OpenGL pixels
//
// Call after:
// Context::Update()
// Context::Render()
// RenderInterface::EndFrame()
//
// Call before SDL_GL_SwapWindow().
// -------------------------------------------------------------

inline bool CheckRenderedGlyphPixels(
    int width,
    int height,
    float display_scale)
{
    Result& result = GetResult();

    result.framebuffer_tested = false;
    result.glyphs_visible = false;

    if (width <= 0 || height <= 0)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Invalid framebuffer dimensions.");

        return false;
    }

    const float scale =
        display_scale > 0.0f ? display_scale : 1.0f;

    const int read_height = std::clamp(
        static_cast<int>(std::ceil(230.0f * scale)),
        1,
        height);

    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(read_height);

    if (w > std::numeric_limits<size_t>::max() / 4 / h)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] Framebuffer allocation overflow.");

        return false;
    }

    std::vector<Uint8> pixels(w * h * 4);

    while (glGetError() != GL_NO_ERROR)
    {
        // Clear previous GL errors before readback.
    }

    GLint old_pack_alignment = 4;

    glGetIntegerv(
        GL_PACK_ALIGNMENT,
        &old_pack_alignment);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glFinish();

    glReadPixels(
        0,
        height - read_height,
        width,
        read_height,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.data());

    glPixelStorei(
        GL_PACK_ALIGNMENT,
        old_pack_alignment);

    const GLenum error = glGetError();

    if (error != GL_NO_ERROR)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "[FONT FAIL] OpenGL readback error: 0x%04X",
            static_cast<unsigned>(error));

        return false;
    }

    result.framebuffer_tested = true;

    // The test panel uses green text (#00FF66).
    size_t matching_pixels = 0;

    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        const unsigned red = pixels[i];
        const unsigned green = pixels[i + 1];
        const unsigned blue = pixels[i + 2];

        if (green >= 60 &&
            green > red + 25 &&
            green > blue + 25)
        {
            ++matching_pixels;
        }
    }

    SDL_Log(
        "[FONT INFO] Framebuffer: %d x %d",
        width,
        height);

    SDL_Log(
        "[FONT INFO] Matching green pixels: %zu",
        matching_pixels);

    result.glyphs_visible =
        matching_pixels > 12;

    if (result.glyphs_visible)
    {
        SDL_Log(
            "[FONT PASS] Glyph-colored pixels detected "
            "in the OpenGL framebuffer.");

        return true;
    }

    SDL_LogError(
        SDL_LOG_CATEGORY_APPLICATION,
        "[FONT FAIL] No sufficient glyph-colored pixels found.");

    SDL_LogError(
        SDL_LOG_CATEGORY_APPLICATION,
        "[FONT DIAGNOSIS] Possible causes:");
    SDL_LogError(
        SDL_LOG_CATEGORY_APPLICATION,
        " - Diagnostic document was not rendered.");
    SDL_LogError(
        SDL_LOG_CATEGORY_APPLICATION,
        " - CSS font-family or font selection problem.");
    SDL_LogError(
        SDL_LOG_CATEGORY_APPLICATION,
        " - Layout, clipping or visibility issue.");
    SDL_LogError(
        SDL_LOG_CATEGORY_APPLICATION,
        " - OpenGL framebuffer/render-interface issue.");

    return false;
}

// -------------------------------------------------------------
// Final report
// -------------------------------------------------------------

inline void PrintFinalReport()
{
    const Result& r = GetResult();

    SDL_Log("==========================================");
    SDL_Log(" ULTRON FONT DIAGNOSTIC REPORT");
    SDL_Log("==========================================");

    SDL_Log("File opened:          %s",
        r.file_opened ? "PASS" : "FAIL");

    SDL_Log("File completely read: %s",
        r.file_read ? "PASS" : "FAIL");

    SDL_Log("Valid signature:      %s",
        r.signature_valid ? "PASS" : "FAIL");

    SDL_Log("FreeType initialized: %s",
        r.freetype_initialized ? "PASS" : "FAIL");

    SDL_Log("FreeType parsed:      %s",
        r.freetype_parsed ? "PASS" : "FAIL");

    SDL_Log("Family matches:       %s",
        r.family_matches ? "PASS" : "FAIL");

    SDL_Log("Unicode available:    %s",
        r.unicode_available ? "PASS" : "FAIL");

    SDL_Log("Glyphs available:     %s",
        r.glyphs_available ? "PASS" : "FAIL");

    SDL_Log("Glyphs rasterized:    %s",
        r.glyphs_rasterized ? "PASS" : "FAIL");

    SDL_Log("RmlUi probe created:  %s",
        r.rml_probe_created ? "PASS" : "FAIL");

    SDL_Log("Framebuffer tested:   %s",
        r.framebuffer_tested ? "PASS" : "NOT TESTED");

    SDL_Log("Glyph pixels visible: %s",
        r.glyphs_visible ? "PASS" : "FAIL / NOT TESTED");

    SDL_Log("==========================================");
}

} // namespace UltronFontDiagnostics
